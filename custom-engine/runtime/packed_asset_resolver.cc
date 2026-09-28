// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/packed_asset_resolver.h"

#include <cstring>
#include <vector>

#include "flutter/assets/asset_hash.h"
#include "flutter/fml/logging.h"
#include "third_party/zlib/zlib.h"

namespace flutter {

namespace {

constexpr uint8_t kMagic[4] = {'F', 'E', 'A', 'P'};

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) {
  return static_cast<uint64_t>(ReadU32LE(p)) |
         (static_cast<uint64_t>(ReadU32LE(p + 4)) << 32);
}

bool RegionInBounds(uint64_t start, uint64_t length, uint64_t total) {
  if (start > total) {
    return false;
  }
  return length <= total - start;
}

PackedAssetCache::Buffer InflateZlibToBuffer(const uint8_t* src,
                                             uint32_t src_size,
                                             uint32_t dest_size) {
  if (dest_size > PackedAssetResolver::kMaxDecompressedSize) {
    FML_DLOG(WARNING) << "PackedAssetResolver: decompressed size too large";
    return nullptr;
  }
  auto out = std::make_shared<std::vector<uint8_t>>(dest_size);
  uLongf produced = dest_size;
  const int rc = uncompress(out->data(), &produced, src, src_size);
  if (rc != Z_OK || produced != dest_size) {
    FML_DLOG(WARNING) << "PackedAssetResolver: inflate failed (" << rc << ")";
    return nullptr;
  }
  return out;
}

std::unique_ptr<fml::Mapping> MappingFromBuffer(PackedAssetCache::Buffer buffer) {
  const uint8_t* data = buffer->data();
  const size_t size = buffer->size();
  return std::make_unique<fml::NonOwnedMapping>(
      data, size, [buffer](const uint8_t*, size_t) {});
}

}  // namespace

uint64_t PackedAssetResolver::HashAssetKey(const std::string& key) {
  return asset_hash_arm64(reinterpret_cast<const uint8_t*>(key.data()),
                          key.size());
}

PackedAssetResolver::PackedAssetResolver(
    const uint8_t* payload,
    size_t size,
    std::shared_ptr<PackedAssetCache> cache,
    const uint8_t* decryption_key,
    const uint8_t* verification_key)
    : payload_(payload), size_(size), cache_(std::move(cache)) {
  if (decryption_key != nullptr) {
    std::memcpy(key_.data(), decryption_key, key_.size());
    has_key_ = true;
  }
  if (verification_key != nullptr) {
    std::memcpy(signature_key_.data(), verification_key,
                signature_key_.size());
    has_signature_key_ = true;
  }
  if (payload_ == nullptr || size_ < kHeaderSize) {
    return;
  }
  if (std::memcmp(payload_, kMagic, sizeof(kMagic)) != 0) {
    FML_DLOG(WARNING) << "PackedAssetResolver: bad magic";
    return;
  }
  const uint16_t version = ReadU16LE(payload_ + 4);
  if (version < 1 || version > kSignedFormatVersion) {
    FML_DLOG(WARNING) << "PackedAssetResolver: unsupported version";
    return;
  }

  count_ = ReadU32LE(payload_ + 8);
  index_offset_ = ReadU32LE(payload_ + 12);
  blob_offset_ = ReadU64LE(payload_ + 16);

  const uint64_t index_bytes =
      static_cast<uint64_t>(count_) * static_cast<uint64_t>(kEntrySize);
  if (index_offset_ != kHeaderSize) {
    FML_DLOG(WARNING) << "PackedAssetResolver: unexpected index offset";
    return;
  }
  if (!RegionInBounds(index_offset_, index_bytes, size_)) {
    FML_DLOG(WARNING) << "PackedAssetResolver: index out of bounds";
    return;
  }
  const uint64_t index_end = index_offset_ + index_bytes;
  if (blob_offset_ < index_end || blob_offset_ > size_) {
    FML_DLOG(WARNING) << "PackedAssetResolver: blob offset out of bounds";
    return;
  }

  const uint16_t header_flags = ReadU16LE(payload_ + 6);
  if ((header_flags & ~(kHeaderFlagEntryCrc32 | kHeaderFlagEncrypted |
                        kHeaderFlagSignedIndex)) != 0) {
    return;
  }
  verify_crc_ = (header_flags & kHeaderFlagEntryCrc32) != 0;
  encrypted_ = (header_flags & kHeaderFlagEncrypted) != 0;
  signed_ = (header_flags & kHeaderFlagSignedIndex) != 0;
  if (signed_ != (version == kSignedFormatVersion) ||
      signed_ != has_signature_key_) {
    return;
  }
  if (signed_) {
    digest_offset_ = ReadU64LE(payload_ + 24);
    const uint64_t digest_bytes =
        static_cast<uint64_t>(count_) * PackedAssetSignature::kDigestSize;
    if (digest_offset_ != index_end ||
        !RegionInBounds(digest_offset_, digest_bytes, size_)) {
      return;
    }
    const uint64_t signature_offset = digest_offset_ + digest_bytes;
    if (!RegionInBounds(signature_offset,
                        PackedAssetSignature::kSignatureSize, size_) ||
        blob_offset_ < signature_offset + PackedAssetSignature::kSignatureSize ||
        !PackedAssetSignature::VerifyIndex(
            payload_, signature_offset, payload_ + signature_offset,
            signature_key_.data())) {
      FML_DLOG(WARNING) << "PackedAssetResolver: index signature invalid";
      return;
    }
  }
  valid_ = true;
}

PackedAssetResolver::~PackedAssetResolver() = default;

bool PackedAssetResolver::IsValid() const {
  return valid_;
}

bool PackedAssetResolver::IsValidAfterAssetManagerChange() const {
  return true;
}

AssetResolver::AssetResolverType PackedAssetResolver::GetType() const {
  return AssetResolver::AssetResolverType::kPackedAssetProvider;
}

bool PackedAssetResolver::ReadEntry(uint32_t index, Entry* out) const {
  const uint64_t pos =
      index_offset_ + static_cast<uint64_t>(index) * kEntrySize;
  if (!RegionInBounds(pos, kEntrySize, size_)) {
    return false;
  }
  const uint8_t* row = payload_ + pos;
  out->key_hash = ReadU64LE(row);
  out->offset = ReadU64LE(row + 8);
  out->stored_size = ReadU32LE(row + 16);
  out->original_size = ReadU32LE(row + 20);
  out->flags = ReadU32LE(row + 24);
  out->checksum = ReadU32LE(row + 28);
  return true;
}

bool PackedAssetResolver::BlockBounds(const Entry& entry,
                                      uint64_t* start) const {
  if (entry.offset > size_ - blob_offset_) {
    return false;
  }
  const uint64_t s = blob_offset_ + entry.offset;
  if (!RegionInBounds(s, entry.stored_size, size_)) {
    return false;
  }
  *start = s;
  return true;
}

bool PackedAssetResolver::Lookup(uint64_t key_hash,
                                 Entry* out,
                                 uint32_t* index) const {
  uint32_t lo = 0;
  uint32_t hi = count_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    Entry probe;
    if (!ReadEntry(mid, &probe)) {
      return false;
    }
    if (probe.key_hash == key_hash) {
      *out = probe;
      *index = mid;
      return true;
    }
    if (probe.key_hash < key_hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

std::unique_ptr<fml::Mapping> PackedAssetResolver::GetAsMapping(
    const std::string& asset_name) const {
  if (!valid_ || asset_name.empty()) {
    return nullptr;
  }

  Entry entry;
  uint32_t index = 0;
  if (!Lookup(HashAssetKey(asset_name), &entry, &index)) {
    return nullptr;
  }

  uint64_t start = 0;
  if (!BlockBounds(entry, &start)) {
    return nullptr;
  }

  if (signed_ &&
      !PackedAssetSignature::BlockMatches(
          payload_ + start, entry.stored_size,
          payload_ + digest_offset_ +
              static_cast<uint64_t>(index) * PackedAssetSignature::kDigestSize)) {
    FML_DLOG(WARNING) << "PackedAssetResolver: asset digest mismatch";
    return nullptr;
  }

  if (verify_crc_) {
    const uint32_t actual = static_cast<uint32_t>(
        crc32(0L, payload_ + start, entry.stored_size));
    if (actual != entry.checksum) {
      FML_DLOG(WARNING) << "PackedAssetResolver: crc32 mismatch";
      return nullptr;
    }
  }

  const uint32_t codec = entry.flags & kCodecMask;
  if (codec != kCodecNone && codec != kCodecZlib) {
    FML_DLOG(WARNING) << "PackedAssetResolver: unknown codec " << codec;
    return nullptr;
  }

  const bool needs_work = encrypted_ || codec != kCodecNone;
  if (!needs_work) {
    return std::make_unique<fml::NonOwnedMapping>(payload_ + start,
                                                  entry.stored_size);
  }

  if (cache_) {
    if (auto cached = cache_->Get(entry.key_hash)) {
      return MappingFromBuffer(std::move(cached));
    }
  }

  const uint8_t* coded_ptr = payload_ + start;
  size_t coded_len = entry.stored_size;
  std::vector<uint8_t> decrypted;
  if (encrypted_) {
    if (!has_key_ || entry.stored_size < PackedAssetCrypto::kNonceSize +
                                             PackedAssetCrypto::kTagSize) {
      return nullptr;
    }
    const uint8_t* nonce = payload_ + start;
    const uint8_t* ct = nonce + PackedAssetCrypto::kNonceSize;
    const size_t ct_len = entry.stored_size - PackedAssetCrypto::kNonceSize;
    if (!PackedAssetCrypto::Open(key_.data(), nonce, ct, ct_len, &decrypted)) {
      FML_DLOG(WARNING) << "PackedAssetResolver: authentication failed";
      return nullptr;
    }
    coded_ptr = decrypted.data();
    coded_len = decrypted.size();
  }

  PackedAssetCache::Buffer final_buffer;
  if (codec == kCodecZlib) {
    final_buffer = InflateZlibToBuffer(
        coded_ptr, static_cast<uint32_t>(coded_len), entry.original_size);
    if (final_buffer == nullptr) {
      return nullptr;
    }
  } else {
    final_buffer =
        std::make_shared<const std::vector<uint8_t>>(std::move(decrypted));
  }

  if (cache_) {
    cache_->Put(entry.key_hash, final_buffer);
  }
  return MappingFromBuffer(std::move(final_buffer));
}

bool PackedAssetResolver::VerifyIntegrity() const {
  if (!valid_) {
    return false;
  }
  for (uint32_t i = 0; i < count_; ++i) {
    Entry entry;
    if (!ReadEntry(i, &entry)) {
      return false;
    }
    uint64_t start = 0;
    if (!BlockBounds(entry, &start)) {
      return false;
    }
    if (signed_ &&
        !PackedAssetSignature::BlockMatches(
            payload_ + start, entry.stored_size,
            payload_ + digest_offset_ +
                static_cast<uint64_t>(i) * PackedAssetSignature::kDigestSize)) {
      return false;
    }
    if (verify_crc_) {
      const uint32_t actual = static_cast<uint32_t>(
          crc32(0L, payload_ + start, entry.stored_size));
      if (actual != entry.checksum) {
        return false;
      }
    }
  }
  return true;
}

bool PackedAssetResolver::operator==(const AssetResolver& other) const {
  if (other.GetType() != GetType()) {
    return false;
  }
  const auto* peer = static_cast<const PackedAssetResolver*>(&other);
  return payload_ == peer->payload_ && size_ == peer->size_;
}

}  // namespace flutter
