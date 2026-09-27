// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/packed_asset_cache.h"

#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {

PackedAssetCache::Buffer MakeBuffer(size_t size, uint8_t fill) {
  return std::make_shared<const std::vector<uint8_t>>(size, fill);
}

}  // namespace

TEST(PackedAssetCacheTest, HitAndMiss) {
  PackedAssetCache cache(1024, 1024);
  EXPECT_EQ(cache.Get(1), nullptr);
  cache.Put(1, MakeBuffer(100, 'a'));
  auto got = cache.Get(1);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->size(), 100u);
  EXPECT_EQ((*got)[0], 'a');

  auto stats = cache.GetStats();
  EXPECT_EQ(stats.hits, 1u);
  EXPECT_EQ(stats.misses, 1u);
  EXPECT_EQ(stats.insertions, 1u);
  EXPECT_EQ(stats.bytes_used, 100u);
}

TEST(PackedAssetCacheTest, EvictsLeastRecentlyUsed) {
  PackedAssetCache cache(250, 250);
  cache.Put(1, MakeBuffer(100, '1'));
  cache.Put(2, MakeBuffer(100, '2'));
  EXPECT_NE(cache.Get(1), nullptr);
  cache.Put(3, MakeBuffer(100, '3'));

  EXPECT_NE(cache.Get(1), nullptr);
  EXPECT_EQ(cache.Get(2), nullptr);
  EXPECT_NE(cache.Get(3), nullptr);

  auto stats = cache.GetStats();
  EXPECT_EQ(stats.evictions, 1u);
  EXPECT_LE(stats.bytes_used, 250u);
}

TEST(PackedAssetCacheTest, SkipsItemsLargerThanMax) {
  PackedAssetCache cache(1024, 128);
  cache.Put(1, MakeBuffer(500, 'x'));
  EXPECT_EQ(cache.Get(1), nullptr);
  auto stats = cache.GetStats();
  EXPECT_EQ(stats.skipped_too_large, 1u);
  EXPECT_EQ(stats.bytes_used, 0u);
}

TEST(PackedAssetCacheTest, UpdatesExistingKey) {
  PackedAssetCache cache(1024, 1024);
  cache.Put(1, MakeBuffer(100, 'a'));
  cache.Put(1, MakeBuffer(200, 'b'));
  auto got = cache.Get(1);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->size(), 200u);
  EXPECT_EQ(cache.GetStats().bytes_used, 200u);
}

TEST(PackedAssetCacheTest, EvictedBufferStaysAliveForHolder) {
  PackedAssetCache cache(150, 150);
  cache.Put(1, MakeBuffer(100, '1'));
  auto held = cache.Get(1);
  ASSERT_NE(held, nullptr);
  cache.Put(2, MakeBuffer(100, '2'));
  EXPECT_EQ(cache.Get(1), nullptr);
  EXPECT_EQ(held->size(), 100u);
  EXPECT_EQ((*held)[0], '1');
}

TEST(PackedAssetCacheTest, ConcurrentAccessDoesNotCrash) {
  PackedAssetCache cache(64 * 1024, 4096);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < 4000; ++i) {
        const uint64_t key = (t * 7 + i) % 64;
        if ((i & 1) == 0) {
          cache.Put(key, MakeBuffer(256, static_cast<uint8_t>(key)));
        } else {
          auto got = cache.Get(key);
          if (got != nullptr) {
            EXPECT_EQ(got->size(), 256u);
          }
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  auto stats = cache.GetStats();
  EXPECT_LE(stats.bytes_used, stats.capacity_bytes);
}

}  // namespace testing
}  // namespace flutter
