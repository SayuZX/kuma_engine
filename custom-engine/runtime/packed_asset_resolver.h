// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_ASSETS_PACKED_ASSET_RESOLVER_H_
#define FLUTTER_ASSETS_PACKED_ASSET_RESOLVER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "flutter/assets/asset_resolver.h"
#include "flutter/assets/packed_asset_cache.h"
#include "flutter/assets/packed_asset_crypto.h"
#include "flutter/assets/packed_asset_signature.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"

namespace flutter {

class PackedAssetResolver final : public AssetResolver {
 public:
  static constexpr uint16_t kFormatVersion = 2;
  static constexpr uint16_t kSignedFormatVersion = 3;
  static constexpr size_t kHeaderSize = 32;
  static constexpr size_t kEntrySize = 32;
  static constexpr uint32_t kCodecMask = 0xff;
  static constexpr uint32_t kCodecNone = 0;
  static constexpr uint32_t kCodecZlib = 1;
  static constexpr uint16_t kHeaderFlagEntryCrc32 = 1u << 0;
  static constexpr uint16_t kHeaderFlagEncrypted = 1u << 1;
  static constexpr uint16_t kHeaderFlagSignedIndex = 1u << 2;
  static constexpr uint64_t kMaxDecompressedSize = 256ull * 1024 * 1024;

  PackedAssetResolver(const uint8_t* payload,
                      size_t size,
                      std::shared_ptr<PackedAssetCache> cache = nullptr,
                      const uint8_t* decryption_key = nullptr,
                      const uint8_t* verification_key = nullptr);

  ~PackedAssetResolver() override;

  static uint64_t HashAssetKey(const std::string& key);

  bool VerifyIntegrity() const;

  PackedAssetCache* cache() const { return cache_.get(); }

  bool IsValid() const override;

  bool IsValidAfterAssetManagerChange() const override;

  AssetResolverType GetType() const override;

  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override;

  bool operator==(const AssetResolver& other) const override;

 private:
  struct Entry {
    uint64_t key_hash = 0;
    uint64_t offset = 0;
    uint32_t stored_size = 0;
    uint32_t original_size = 0;
    uint32_t flags = 0;
    uint32_t checksum = 0;
  };

  bool ReadEntry(uint32_t index, Entry* out) const;

  bool Lookup(uint64_t key_hash, Entry* out, uint32_t* index) const;

  bool BlockBounds(const Entry& entry, uint64_t* start) const;

  const uint8_t* const payload_;
  const size_t size_;
  std::shared_ptr<PackedAssetCache> cache_;
  std::array<uint8_t, PackedAssetCrypto::kKeySize> key_{};
  std::array<uint8_t, PackedAssetSignature::kPublicKeySize> signature_key_{};
  bool has_key_ = false;
  bool has_signature_key_ = false;
  bool valid_ = false;
  bool verify_crc_ = false;
  bool encrypted_ = false;
  bool signed_ = false;
  uint32_t count_ = 0;
  uint64_t index_offset_ = 0;
  uint64_t blob_offset_ = 0;
  uint64_t digest_offset_ = 0;

  FML_DISALLOW_COPY_AND_ASSIGN(PackedAssetResolver);
};

}  // namespace flutter

#endif  // FLUTTER_ASSETS_PACKED_ASSET_RESOLVER_H_
