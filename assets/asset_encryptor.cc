// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <openssl/sha.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "flutter/assets/packed_asset_crypto.h"
#include "flutter/assets/packed_asset_resolver.h"
#include "third_party/zlib/zlib.h"

namespace {

constexpr size_t kHeaderSize = 32;
constexpr size_t kEntrySize = 32;

uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t ReadU64(const uint8_t* p) {
  return static_cast<uint64_t>(ReadU32(p)) |
         (static_cast<uint64_t>(ReadU32(p + 4)) << 32);
}
void PushU16(std::vector<uint8_t>* o, uint16_t v) {
  o->push_back(v & 0xff);
  o->push_back((v >> 8) & 0xff);
}
void PushU32(std::vector<uint8_t>* o, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    o->push_back((v >> (8 * i)) & 0xff);
  }
}
void PushU64(std::vector<uint8_t>* o, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    o->push_back((v >> (8 * i)) & 0xff);
  }
}

uint64_t AlignUp(uint64_t v, uint64_t a) {
  const uint64_t r = v % a;
  return r == 0 ? v : v + (a - r);
}

bool ReadFile(const char* path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out->resize(size < 0 ? 0 : static_cast<size_t>(size));
  const size_t read = std::fread(out->data(), 1, out->size(), f);
  std::fclose(f);
  return read == out->size();
}

bool HexToKey(const std::string& hex, uint8_t key[32]) {
  if (hex.size() != 64) {
    return false;
  }
  for (size_t i = 0; i < 32; ++i) {
    unsigned int byte = 0;
    if (std::sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) {
      return false;
    }
    key[i] = static_cast<uint8_t>(byte);
  }
  return true;
}

struct Entry {
  uint64_t key_hash;
  uint32_t original_size;
  uint32_t codec;
  std::vector<uint8_t> stored;
};

}  // namespace

int main(int argc, char** argv) {
  const char* in_path = nullptr;
  const char* out_path = nullptr;
  std::string key_hex;
  uint64_t alignment = 16;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--input" && i + 1 < argc) {
      in_path = argv[++i];
    } else if (a == "--output" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--key" && i + 1 < argc) {
      key_hex = argv[++i];
    } else if (a == "--alignment" && i + 1 < argc) {
      alignment = std::strtoull(argv[++i], nullptr, 10);
    }
  }
  if (in_path == nullptr || out_path == nullptr || key_hex.empty()) {
    std::fprintf(stderr,
                 "usage: asset_encryptor --input <payload.bin> --output "
                 "<enc.bin> --key <64hex> [--alignment N]\n");
    return 2;
  }
  uint8_t key[32];
  if (!HexToKey(key_hex, key)) {
    std::fprintf(stderr, "invalid --key (need 64 hex chars)\n");
    return 2;
  }

  std::vector<uint8_t> in;
  if (!ReadFile(in_path, &in) || in.size() < kHeaderSize) {
    std::fprintf(stderr, "cannot read input\n");
    return 2;
  }
  if (std::memcmp(in.data(), "FEAP", 4) != 0) {
    std::fprintf(stderr, "bad magic\n");
    return 1;
  }
  const uint32_t count = ReadU32(in.data() + 8);
  const uint32_t index_offset = ReadU32(in.data() + 12);
  const uint64_t blob_offset = ReadU64(in.data() + 16);

  std::vector<Entry> entries;
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* row = in.data() + index_offset + i * kEntrySize;
    Entry e;
    e.key_hash = ReadU64(row);
    const uint64_t offset = ReadU64(row + 8);
    const uint32_t stored_size = ReadU32(row + 16);
    e.original_size = ReadU32(row + 20);
    e.codec = ReadU32(row + 24);
    const uint8_t* block = in.data() + blob_offset + offset;

    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha;
    SHA256_Init(&sha);
    SHA256_Update(&sha, key, sizeof(key));
    SHA256_Update(&sha, block, stored_size);
    SHA256_Final(hash, &sha);

    std::vector<uint8_t> sealed;
    if (!flutter::PackedAssetCrypto::Seal(key, hash, block, stored_size,
                                          &sealed)) {
      std::fprintf(stderr, "seal failed for entry %u\n", i);
      return 1;
    }
    e.stored.reserve(flutter::PackedAssetCrypto::kNonceSize + sealed.size());
    e.stored.insert(e.stored.end(), hash,
                    hash + flutter::PackedAssetCrypto::kNonceSize);
    e.stored.insert(e.stored.end(), sealed.begin(), sealed.end());
    entries.push_back(std::move(e));
  }

  const uint64_t new_index_offset = kHeaderSize;
  const uint64_t new_index_end = new_index_offset + count * kEntrySize;
  const uint64_t new_blob_offset = AlignUp(new_index_end, alignment);

  std::vector<uint64_t> offsets(count);
  uint64_t cursor = 0;
  for (uint32_t i = 0; i < count; ++i) {
    cursor = AlignUp(cursor, alignment);
    offsets[i] = cursor;
    cursor += entries[i].stored.size();
  }
  const uint64_t blob_size = cursor;

  std::vector<uint8_t> out(new_blob_offset + blob_size, 0);
  out[0] = 'F';
  out[1] = 'E';
  out[2] = 'A';
  out[3] = 'P';
  std::vector<uint8_t> header;
  PushU16(&header, flutter::PackedAssetResolver::kFormatVersion);
  PushU16(&header, flutter::PackedAssetResolver::kHeaderFlagEntryCrc32 |
                       flutter::PackedAssetResolver::kHeaderFlagEncrypted);
  PushU32(&header, count);
  PushU32(&header, static_cast<uint32_t>(new_index_offset));
  PushU64(&header, new_blob_offset);
  PushU64(&header, 0);
  std::memcpy(out.data() + 4, header.data(), header.size());

  for (uint32_t i = 0; i < count; ++i) {
    std::vector<uint8_t> row;
    PushU64(&row, entries[i].key_hash);
    PushU64(&row, offsets[i]);
    PushU32(&row, static_cast<uint32_t>(entries[i].stored.size()));
    PushU32(&row, entries[i].original_size);
    PushU32(&row, entries[i].codec);
    const uint32_t crc = static_cast<uint32_t>(
        crc32(0L, entries[i].stored.data(),
              static_cast<uInt>(entries[i].stored.size())));
    PushU32(&row, crc);
    std::memcpy(out.data() + new_index_offset + i * kEntrySize, row.data(),
                row.size());
    std::memcpy(out.data() + new_blob_offset + offsets[i],
                entries[i].stored.data(), entries[i].stored.size());
  }

  FILE* f = std::fopen(out_path, "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot write output\n");
    return 1;
  }
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  std::fprintf(stdout, "encrypted %u assets -> %s (%zu bytes)\n", count,
               out_path, out.size());
  return 0;
}
