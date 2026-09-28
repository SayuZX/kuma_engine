// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/packed_asset_resolver.h"

#include <openssl/curve25519.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "third_party/zlib/zlib.h"

namespace flutter {
namespace testing {

namespace {

void AppendU16LE(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>(v & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void AppendU32LE(std::vector<uint8_t>* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

void AppendU64LE(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

struct BuiltEntry {
  uint64_t key_hash;
  uint64_t offset;
  uint32_t stored_size;
  uint32_t original_size;
  uint32_t flags;
};

std::vector<uint8_t> BuildPayload(
    const std::map<std::string, std::string>& assets) {
  std::vector<BuiltEntry> entries;
  std::vector<uint8_t> blob;
  for (const auto& kv : assets) {
    BuiltEntry e;
    e.key_hash = PackedAssetResolver::HashAssetKey(kv.first);
    e.offset = blob.size();
    e.stored_size = static_cast<uint32_t>(kv.second.size());
    e.original_size = e.stored_size;
    e.flags = 0;
    blob.insert(blob.end(), kv.second.begin(), kv.second.end());
    entries.push_back(e);
  }
  std::sort(entries.begin(), entries.end(),
            [](const BuiltEntry& a, const BuiltEntry& b) {
              return a.key_hash < b.key_hash;
            });

  const uint32_t count = static_cast<uint32_t>(entries.size());
  const uint64_t index_offset = PackedAssetResolver::kHeaderSize;
  const uint64_t blob_offset =
      index_offset + count * PackedAssetResolver::kEntrySize;

  std::vector<uint8_t> out;
  out.push_back('F');
  out.push_back('E');
  out.push_back('A');
  out.push_back('P');
  AppendU16LE(&out, PackedAssetResolver::kFormatVersion);
  AppendU16LE(&out, 0);
  AppendU32LE(&out, count);
  AppendU32LE(&out, static_cast<uint32_t>(index_offset));
  AppendU64LE(&out, blob_offset);
  AppendU64LE(&out, 0);

  for (const auto& e : entries) {
    AppendU64LE(&out, e.key_hash);
    AppendU64LE(&out, e.offset);
    AppendU32LE(&out, e.stored_size);
    AppendU32LE(&out, e.original_size);
    AppendU32LE(&out, e.flags);
    AppendU32LE(&out, 0);
  }
  out.insert(out.end(), blob.begin(), blob.end());
  return out;
}

std::string MappingToString(const fml::Mapping& mapping) {
  return std::string(reinterpret_cast<const char*>(mapping.GetMapping()),
                     mapping.GetSize());
}

std::vector<uint8_t> ZlibCompress(const std::string& data) {
  uLongf bound = compressBound(data.size());
  std::vector<uint8_t> out(bound);
  int rc = compress(out.data(), &bound,
                    reinterpret_cast<const Bytef*>(data.data()), data.size());
  EXPECT_EQ(rc, Z_OK);
  out.resize(bound);
  return out;
}

struct CodecEntry {
  std::string key;
  std::vector<uint8_t> stored;
  uint32_t original_size;
  uint32_t codec;
};

std::vector<uint8_t> SealBlock(const uint8_t key[32],
                               const uint8_t nonce[12],
                               const std::vector<uint8_t>& coded) {
  std::vector<uint8_t> sealed;
  EXPECT_TRUE(PackedAssetCrypto::Seal(key, nonce, coded.data(), coded.size(),
                                      &sealed));
  std::vector<uint8_t> stored(nonce, nonce + 12);
  stored.insert(stored.end(), sealed.begin(), sealed.end());
  return stored;
}

std::vector<uint8_t> BuildCodecPayload(std::vector<CodecEntry> entries,
                                       bool with_crc = false,
                                       bool encrypted = false) {
  std::sort(entries.begin(), entries.end(),
            [](const CodecEntry& a, const CodecEntry& b) {
              return PackedAssetResolver::HashAssetKey(a.key) <
                     PackedAssetResolver::HashAssetKey(b.key);
            });
  const uint32_t count = static_cast<uint32_t>(entries.size());
  const uint64_t index_offset = PackedAssetResolver::kHeaderSize;
  const uint64_t blob_offset =
      index_offset + count * PackedAssetResolver::kEntrySize;

  std::vector<uint8_t> blob;
  std::vector<uint64_t> offsets;
  for (const auto& e : entries) {
    offsets.push_back(blob.size());
    blob.insert(blob.end(), e.stored.begin(), e.stored.end());
  }

  std::vector<uint8_t> out;
  out.push_back('F');
  out.push_back('E');
  out.push_back('A');
  out.push_back('P');
  uint16_t header_flags = 0;
  if (with_crc) {
    header_flags |= PackedAssetResolver::kHeaderFlagEntryCrc32;
  }
  if (encrypted) {
    header_flags |= PackedAssetResolver::kHeaderFlagEncrypted;
  }
  AppendU16LE(&out, PackedAssetResolver::kFormatVersion);
  AppendU16LE(&out, header_flags);
  AppendU32LE(&out, count);
  AppendU32LE(&out, static_cast<uint32_t>(index_offset));
  AppendU64LE(&out, blob_offset);
  AppendU64LE(&out, 0);
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    AppendU64LE(&out, PackedAssetResolver::HashAssetKey(e.key));
    AppendU64LE(&out, offsets[i]);
    AppendU32LE(&out, static_cast<uint32_t>(e.stored.size()));
    AppendU32LE(&out, e.original_size);
    AppendU32LE(&out, e.codec);
    uint32_t checksum = 0;
    if (with_crc) {
      checksum = static_cast<uint32_t>(
          crc32(0L, e.stored.data(), static_cast<uInt>(e.stored.size())));
    }
    AppendU32LE(&out, checksum);
  }
  out.insert(out.end(), blob.begin(), blob.end());
  return out;
}

std::vector<uint8_t> BuildSignedPayload(
    const std::vector<uint8_t>& input,
    const uint8_t private_key[PackedAssetSignature::kPrivateKeySize]) {
  const uint32_t count = static_cast<uint32_t>(input[8]) |
                         (static_cast<uint32_t>(input[9]) << 8) |
                         (static_cast<uint32_t>(input[10]) << 16) |
                         (static_cast<uint32_t>(input[11]) << 24);
  const size_t index_end = PackedAssetResolver::kHeaderSize +
                           static_cast<size_t>(count) *
                               PackedAssetResolver::kEntrySize;
  const size_t signature_offset =
      index_end + count * PackedAssetSignature::kDigestSize;
  const size_t new_blob =
      (signature_offset + PackedAssetSignature::kSignatureSize + 15) & ~15u;
  const size_t old_blob = index_end;
  std::vector<uint8_t> out(new_blob + input.size() - old_blob, 0);
  std::memcpy(out.data(), input.data(), index_end);
  out[4] = PackedAssetResolver::kSignedFormatVersion;
  out[5] = 0;
  out[6] = PackedAssetResolver::kHeaderFlagSignedIndex;
  out[7] = 0;
  for (size_t i = 0; i < 8; ++i) {
    out[16 + i] = static_cast<uint8_t>(new_blob >> (8 * i));
    out[24 + i] = static_cast<uint8_t>(index_end >> (8 * i));
  }
  for (uint32_t i = 0; i < count; ++i) {
    const size_t row = PackedAssetResolver::kHeaderSize +
                       static_cast<size_t>(i) * PackedAssetResolver::kEntrySize;
    size_t offset = 0;
    for (size_t byte = 0; byte < 8; ++byte) {
      offset |= static_cast<size_t>(out[row + 8 + byte]) << (8 * byte);
    }
    const size_t stored = static_cast<size_t>(out[row + 16]) |
                          (static_cast<size_t>(out[row + 17]) << 8) |
                          (static_cast<size_t>(out[row + 18]) << 16) |
                          (static_cast<size_t>(out[row + 19]) << 24);
    PackedAssetSignature::DigestBlock(
        input.data() + old_blob + offset, stored,
        out.data() + index_end +
            static_cast<size_t>(i) * PackedAssetSignature::kDigestSize);
    std::memset(out.data() + row + 28, 0, 4);
  }
  EXPECT_TRUE(PackedAssetSignature::SignIndex(
      out.data(), signature_offset, private_key,
      out.data() + signature_offset));
  std::memcpy(out.data() + new_blob, input.data() + old_blob,
              input.size() - old_blob);
  return out;
}

}  // namespace

TEST(PackedAssetResolverTest, ResolvesSingleUncompressedAsset) {
  const std::string key = "assets/hello.txt";
  const std::string value = "Hello from packed payload!\n";
  std::vector<uint8_t> payload = BuildPayload({{key, value}});

  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetType(),
            AssetResolver::AssetResolverType::kPackedAssetProvider);

  auto mapping = resolver.GetAsMapping(key);
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(mapping->GetSize(), value.size());
  EXPECT_EQ(MappingToString(*mapping), value);
}

TEST(PackedAssetResolverTest, ResolvesAmongMultipleAssets) {
  std::map<std::string, std::string> assets = {
      {"assets/a.txt", "alpha"},
      {"assets/b.json", "{\"k\":1}"},
      {"AssetManifest.bin", "manifest-bytes"},
      {"assets/nested/deep/c.dat", std::string(1024, 'z')},
  };
  std::vector<uint8_t> payload = BuildPayload(assets);

  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());

  for (const auto& kv : assets) {
    auto mapping = resolver.GetAsMapping(kv.first);
    ASSERT_NE(mapping, nullptr) << "missing " << kv.first;
    EXPECT_EQ(MappingToString(*mapping), kv.second) << "mismatch " << kv.first;
  }
}

TEST(PackedAssetResolverTest, MissingKeyReturnsNull) {
  std::vector<uint8_t> payload = BuildPayload({{"assets/a.txt", "alpha"}});
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping("assets/does_not_exist.txt"), nullptr);
}

TEST(PackedAssetResolverTest, EmptyNameReturnsNull) {
  std::vector<uint8_t> payload = BuildPayload({{"assets/a.txt", "alpha"}});
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping(""), nullptr);
}

TEST(PackedAssetResolverTest, NullOrTooSmallPayloadIsInvalid) {
  PackedAssetResolver null_resolver(nullptr, 0);
  EXPECT_FALSE(null_resolver.IsValid());

  std::vector<uint8_t> tiny(8, 0);
  PackedAssetResolver tiny_resolver(tiny.data(), tiny.size());
  EXPECT_FALSE(tiny_resolver.IsValid());
}

TEST(PackedAssetResolverTest, BadMagicIsInvalid) {
  std::vector<uint8_t> payload = BuildPayload({{"assets/a.txt", "alpha"}});
  payload[0] = 'X';
  PackedAssetResolver resolver(payload.data(), payload.size());
  EXPECT_FALSE(resolver.IsValid());
}

TEST(PackedAssetResolverTest, TruncatedIndexIsInvalid) {
  std::vector<uint8_t> payload = BuildPayload(
      {{"assets/a.txt", "alpha"}, {"assets/b.txt", "beta"}});
  payload.resize(PackedAssetResolver::kHeaderSize + 4);
  PackedAssetResolver resolver(payload.data(), payload.size());
  EXPECT_FALSE(resolver.IsValid());
}

TEST(PackedAssetResolverTest, CorruptEntryOffsetReturnsNullNoCrash) {
  const std::string key = "assets/a.txt";
  std::vector<uint8_t> payload = BuildPayload({{key, "alpha"}});
  const size_t entry0 = PackedAssetResolver::kHeaderSize;
  const uint64_t huge = 0xffffffffffffff00ull;
  for (int i = 0; i < 8; ++i) {
    payload[entry0 + 8 + i] = static_cast<uint8_t>((huge >> (8 * i)) & 0xff);
  }
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping(key), nullptr);
}

TEST(PackedAssetResolverTest, InflatesZlibAsset) {
  const std::string key = "assets/big.json";
  std::string value;
  for (int i = 0; i < 400; ++i) {
    value += "{\"repeated\":\"payload\",\"i\":" + std::to_string(i) + "}\n";
  }
  auto compressed = ZlibCompress(value);
  ASSERT_LT(compressed.size(), value.size());

  auto payload = BuildCodecPayload({{key, compressed,
                                     static_cast<uint32_t>(value.size()),
                                     PackedAssetResolver::kCodecZlib}});
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());

  auto mapping = resolver.GetAsMapping(key);
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(mapping->GetSize(), value.size());
  EXPECT_EQ(MappingToString(*mapping), value);
}

TEST(PackedAssetResolverTest, MixedCodecsResolveIndependently) {
  const std::string raw = "plain-bytes";
  const std::string comp_src = std::string(500, 'a') + std::string(500, 'b');
  auto payload = BuildCodecPayload({
      {"assets/raw.bin",
       std::vector<uint8_t>(raw.begin(), raw.end()),
       static_cast<uint32_t>(raw.size()), PackedAssetResolver::kCodecNone},
      {"assets/comp.txt", ZlibCompress(comp_src),
       static_cast<uint32_t>(comp_src.size()),
       PackedAssetResolver::kCodecZlib},
  });
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping("assets/raw.bin")), raw);
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping("assets/comp.txt")),
            comp_src);
}

TEST(PackedAssetResolverTest, UnknownCodecReturnsNull) {
  const std::string key = "assets/x.bin";
  std::vector<uint8_t> stored = {1, 2, 3, 4};
  auto payload = BuildCodecPayload({{key, stored, 4, 7}});
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping(key), nullptr);
}

TEST(PackedAssetResolverTest, CorruptCompressedDataReturnsNullNoCrash) {
  const std::string key = "assets/c.txt";
  std::string value = std::string(600, 'z');
  auto compressed = ZlibCompress(value);
  for (size_t i = 4; i < compressed.size(); ++i) {
    compressed[i] ^= 0xff;
  }
  auto payload = BuildCodecPayload({{key, compressed,
                                     static_cast<uint32_t>(value.size()),
                                     PackedAssetResolver::kCodecZlib}});
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping(key), nullptr);
}

TEST(PackedAssetResolverTest, AcceptsLegacyVersionOne) {
  auto payload = BuildPayload({{"assets/a.txt", "alpha"}});
  payload[4] = 1;
  payload[5] = 0;
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping("assets/a.txt")), "alpha");
}

TEST(PackedAssetResolverTest, ValidCrcResolvesAndVerifies) {
  const std::string k1 = "assets/a.txt";
  const std::string v1 = "alpha-bytes";
  const std::string k2 = "assets/b.bin";
  const std::string v2 = std::string(300, 'q');
  auto payload = BuildCodecPayload(
      {
          {k1, std::vector<uint8_t>(v1.begin(), v1.end()),
           static_cast<uint32_t>(v1.size()), PackedAssetResolver::kCodecNone},
          {k2, std::vector<uint8_t>(v2.begin(), v2.end()),
           static_cast<uint32_t>(v2.size()), PackedAssetResolver::kCodecNone},
      },
      /*with_crc=*/true);
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_TRUE(resolver.VerifyIntegrity());
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping(k1)), v1);
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping(k2)), v2);
}

TEST(PackedAssetResolverTest, CorruptedBlockFailsCrc) {
  const std::string key = "assets/a.txt";
  const std::string value = std::string(200, 'm');
  auto payload = BuildCodecPayload(
      {{key, std::vector<uint8_t>(value.begin(), value.end()),
        static_cast<uint32_t>(value.size()), PackedAssetResolver::kCodecNone}},
      /*with_crc=*/true);
  payload.back() ^= 0x01;

  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_FALSE(resolver.VerifyIntegrity());
  EXPECT_EQ(resolver.GetAsMapping(key), nullptr);
}

TEST(PackedAssetResolverTest, LegacyPayloadWithoutCrcVerifiesTrivially) {
  auto payload = BuildPayload({{"assets/a.txt", "alpha"}});
  PackedAssetResolver resolver(payload.data(), payload.size());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_TRUE(resolver.VerifyIntegrity());
}

TEST(PackedAssetResolverTest, CacheServesCompressedAssetOnSecondRead) {
  const std::string key = "assets/big.txt";
  const std::string value = std::string(4000, 'k');
  auto payload = BuildCodecPayload(
      {{key, ZlibCompress(value), static_cast<uint32_t>(value.size()),
        PackedAssetResolver::kCodecZlib}},
      /*with_crc=*/true);

  auto cache = std::make_shared<PackedAssetCache>(1024 * 1024, 1024 * 1024);
  PackedAssetResolver resolver(payload.data(), payload.size(), cache);
  ASSERT_TRUE(resolver.IsValid());

  auto first = resolver.GetAsMapping(key);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(MappingToString(*first), value);
  EXPECT_EQ(cache->GetStats().misses, 1u);
  EXPECT_EQ(cache->GetStats().insertions, 1u);

  auto second = resolver.GetAsMapping(key);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(MappingToString(*second), value);
  EXPECT_EQ(cache->GetStats().hits, 1u);

  EXPECT_EQ(first->GetMapping(), second->GetMapping());
}

TEST(PackedAssetResolverTest, CacheNotUsedForUncompressedAssets) {
  const std::string key = "assets/raw.bin";
  const std::string value = "uncompressed";
  auto payload = BuildCodecPayload(
      {{key, std::vector<uint8_t>(value.begin(), value.end()),
        static_cast<uint32_t>(value.size()), PackedAssetResolver::kCodecNone}},
      /*with_crc=*/true);
  auto cache = std::make_shared<PackedAssetCache>(1024 * 1024, 1024 * 1024);
  PackedAssetResolver resolver(payload.data(), payload.size(), cache);
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping(key)), value);
  auto stats = cache->GetStats();
  EXPECT_EQ(stats.insertions, 0u);
  EXPECT_EQ(stats.hits, 0u);
  EXPECT_EQ(stats.misses, 0u);
}

TEST(PackedAssetResolverTest, DecryptsEncryptedAssetWithKey) {
  std::array<uint8_t, 32> key{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(i + 1);
  }
  std::array<uint8_t, 12> nonce{};
  nonce[0] = 9;
  const std::string key_name = "assets/secret.bin";
  const std::string value = "top-secret-asset-contents";
  auto stored =
      SealBlock(key.data(), nonce.data(),
                std::vector<uint8_t>(value.begin(), value.end()));
  auto payload = BuildCodecPayload(
      {{key_name, stored, static_cast<uint32_t>(value.size()),
        PackedAssetResolver::kCodecNone}},
      /*with_crc=*/true, /*encrypted=*/true);

  PackedAssetResolver ok(payload.data(), payload.size(), nullptr, key.data());
  ASSERT_TRUE(ok.IsValid());
  auto mapping = ok.GetAsMapping(key_name);
  ASSERT_NE(mapping, nullptr);
  EXPECT_EQ(MappingToString(*mapping), value);

  PackedAssetResolver no_key(payload.data(), payload.size());
  ASSERT_TRUE(no_key.IsValid());
  EXPECT_EQ(no_key.GetAsMapping(key_name), nullptr);
}

TEST(PackedAssetResolverTest, WrongKeyFailsAuthentication) {
  std::array<uint8_t, 32> key{};
  key[0] = 1;
  std::array<uint8_t, 32> wrong{};
  wrong[0] = 2;
  std::array<uint8_t, 12> nonce{};
  const std::string key_name = "assets/secret.bin";
  const std::string value = std::string(200, 's');
  auto stored =
      SealBlock(key.data(), nonce.data(),
                std::vector<uint8_t>(value.begin(), value.end()));
  auto payload = BuildCodecPayload(
      {{key_name, stored, static_cast<uint32_t>(value.size()),
        PackedAssetResolver::kCodecNone}},
      /*with_crc=*/true, /*encrypted=*/true);

  PackedAssetResolver resolver(payload.data(), payload.size(), nullptr,
                               wrong.data());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping(key_name), nullptr);
}

TEST(PackedAssetResolverTest, TamperedCiphertextFailsAuthentication) {
  std::array<uint8_t, 32> key{};
  key[0] = 7;
  std::array<uint8_t, 12> nonce{};
  const std::string key_name = "assets/secret.bin";
  const std::string value = std::string(200, 't');
  auto stored =
      SealBlock(key.data(), nonce.data(),
                std::vector<uint8_t>(value.begin(), value.end()));
  auto payload = BuildCodecPayload(
      {{key_name, stored, static_cast<uint32_t>(value.size()),
        PackedAssetResolver::kCodecNone}},
      /*with_crc=*/false, /*encrypted=*/true);
  payload.back() ^= 0x40;

  PackedAssetResolver resolver(payload.data(), payload.size(), nullptr,
                               key.data());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping(key_name), nullptr);
}

TEST(PackedAssetResolverTest, EncryptedThenCompressedRoundTrips) {
  std::array<uint8_t, 32> key{};
  key[0] = 3;
  key[1] = 5;
  std::array<uint8_t, 12> nonce{};
  nonce[0] = 1;
  const std::string key_name = "assets/big.json";
  const std::string value = std::string(3000, 'z');
  auto compressed = ZlibCompress(value);
  ASSERT_LT(compressed.size(), value.size());
  auto stored = SealBlock(key.data(), nonce.data(), compressed);
  auto payload = BuildCodecPayload(
      {{key_name, stored, static_cast<uint32_t>(value.size()),
        PackedAssetResolver::kCodecZlib}},
      /*with_crc=*/true, /*encrypted=*/true);

  auto cache = std::make_shared<PackedAssetCache>(1024 * 1024, 1024 * 1024);
  PackedAssetResolver resolver(payload.data(), payload.size(), cache,
                               key.data());
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping(key_name)), value);
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping(key_name)), value);
  EXPECT_EQ(cache->GetStats().hits, 1u);
}

TEST(PackedAssetResolverTest, SignedIndexLoadsAssetsOffline) {
  uint8_t public_key[32];
  uint8_t private_key[64];
  ED25519_keypair(public_key, private_key);
  auto unsigned_payload = BuildPayload({{"assets/a.txt", "alpha"},
                                        {"assets/b.txt", "bravo"}});
  auto signed_payload = BuildSignedPayload(unsigned_payload, private_key);
  PackedAssetResolver resolver(signed_payload.data(), signed_payload.size(),
                               nullptr, nullptr, public_key);
  ASSERT_TRUE(resolver.IsValid());
  ASSERT_NE(resolver.GetAsMapping("assets/a.txt"), nullptr);
  EXPECT_EQ(MappingToString(*resolver.GetAsMapping("assets/a.txt")), "alpha");
  EXPECT_TRUE(resolver.VerifyIntegrity());
  PackedAssetResolver no_key(signed_payload.data(), signed_payload.size());
  EXPECT_FALSE(no_key.IsValid());
  PackedAssetResolver unsigned_with_key(unsigned_payload.data(),
                                        unsigned_payload.size(), nullptr,
                                        nullptr, public_key);
  EXPECT_FALSE(unsigned_with_key.IsValid());
}

TEST(PackedAssetResolverTest, SignedIndexRejectsMetadataTampering) {
  uint8_t public_key[32];
  uint8_t private_key[64];
  ED25519_keypair(public_key, private_key);
  auto input = BuildPayload({{"assets/a.txt", "alpha"}});
  auto signed_payload = BuildSignedPayload(input, private_key);
  signed_payload[PackedAssetResolver::kHeaderSize + 24] ^= 1;
  PackedAssetResolver resolver(signed_payload.data(), signed_payload.size(),
                               nullptr, nullptr, public_key);
  EXPECT_FALSE(resolver.IsValid());
  signed_payload[PackedAssetResolver::kHeaderSize + 24] ^= 1;
  public_key[0] ^= 1;
  PackedAssetResolver wrong_key(signed_payload.data(), signed_payload.size(),
                                 nullptr, nullptr, public_key);
  EXPECT_FALSE(wrong_key.IsValid());
}

TEST(PackedAssetResolverTest, SignedIndexRejectsBlockTamperingLazily) {
  uint8_t public_key[32];
  uint8_t private_key[64];
  ED25519_keypair(public_key, private_key);
  auto input = BuildPayload({{"assets/a.txt", "alpha"}});
  auto signed_payload = BuildSignedPayload(input, private_key);
  signed_payload.back() ^= 1;
  PackedAssetResolver resolver(signed_payload.data(), signed_payload.size(),
                               nullptr, nullptr, public_key);
  ASSERT_TRUE(resolver.IsValid());
  EXPECT_EQ(resolver.GetAsMapping("assets/a.txt"), nullptr);
  EXPECT_FALSE(resolver.VerifyIntegrity());
}

TEST(PackedAssetResolverTest, FuzzMalformedPayloadNeverCrashes) {
  std::map<std::string, std::string> assets = {
      {"assets/a.txt", "alpha"},
      {"assets/b.json", "{\"k\":1}"},
      {"assets/c.dat", std::string(64, 'x')},
  };
  const std::vector<uint8_t> base = BuildPayload(assets);

  std::mt19937 rng(1234);
  for (int iter = 0; iter < 5000; ++iter) {
    std::vector<uint8_t> p = base;
    const int flips = 1 + (rng() % 8);
    for (int f = 0; f < flips; ++f) {
      p[rng() % p.size()] = static_cast<uint8_t>(rng() & 0xff);
    }
    if (!p.empty() && (rng() & 1)) {
      p.resize(1 + (rng() % p.size()));
    }
    PackedAssetResolver resolver(p.data(), p.size());
    for (const auto& kv : assets) {
      auto mapping = resolver.GetAsMapping(kv.first);
      if (mapping != nullptr) {
        EXPECT_LE(mapping->GetSize(), p.size());
      }
    }
    resolver.GetAsMapping("assets/not/present.bin");
    resolver.VerifyIntegrity();
  }
}

}  // namespace testing
}  // namespace flutter
