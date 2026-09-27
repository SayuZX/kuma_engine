// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/asset_hash.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

TEST(AssetHashTest, MatchesKnownFnv1a64Vectors) {
  auto h = [](const std::string& s) {
    return asset_hash_arm64(reinterpret_cast<const uint8_t*>(s.data()),
                            s.size());
  };
  EXPECT_EQ(h(""), 0xcbf29ce484222325ull);
  EXPECT_EQ(h("a"), 0xaf63dc4c8601ec8cull);
  EXPECT_EQ(h("foobar"), 0x85944171f73967e8ull);
}

TEST(AssetHashTest, ArmApiMatchesReferenceOnRandomInputs) {
  std::mt19937 rng(20240927);
  std::vector<uint8_t> buf;
  for (int i = 0; i < 100000; ++i) {
    const size_t len = rng() % 128;
    buf.resize(len);
    for (size_t j = 0; j < len; ++j) {
      buf[j] = static_cast<uint8_t>(rng() & 0xff);
    }
    EXPECT_EQ(asset_hash_arm64(buf.data(), buf.size()),
              AssetHashReference(buf.data(), buf.size()));
  }
}

TEST(AssetHashTest, ThroughputDiagnostic) {
  std::vector<uint8_t> buf(1 << 20, 0xab);
  const int iterations = 256;
  uint64_t sink = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    buf[i] = static_cast<uint8_t>(i + 1);
    sink += asset_hash_arm64(buf.data(), buf.size());
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(end - begin).count();
  const double mb = static_cast<double>(iterations) * buf.size() / (1 << 20);
  std::fprintf(stderr, "[   INFO   ] asset_hash_arm64 throughput: %.0f MB/s\n",
               mb / seconds);
  EXPECT_GT(sink, 0u);
}

}  // namespace testing
}  // namespace flutter
