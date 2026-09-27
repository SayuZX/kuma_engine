// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/packed_asset_cache.h"

#include <utility>

namespace flutter {

PackedAssetCache::PackedAssetCache(size_t capacity_bytes, size_t max_item_bytes)
    : capacity_bytes_(capacity_bytes), max_item_bytes_(max_item_bytes) {}

PackedAssetCache::Buffer PackedAssetCache::Get(uint64_t key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = index_.find(key);
  if (it == index_.end()) {
    ++misses_;
    return nullptr;
  }
  lru_.splice(lru_.begin(), lru_, it->second);
  ++hits_;
  return it->second->value;
}

void PackedAssetCache::EvictLocked(size_t incoming) {
  while (!lru_.empty() && bytes_used_ + incoming > capacity_bytes_) {
    const Node& victim = lru_.back();
    bytes_used_ -= victim.size;
    index_.erase(victim.key);
    lru_.pop_back();
    ++evictions_;
  }
}

void PackedAssetCache::Put(uint64_t key, Buffer value) {
  if (value == nullptr) {
    return;
  }
  const size_t size = value->size();

  std::lock_guard<std::mutex> lock(mutex_);
  if (size > max_item_bytes_ || size > capacity_bytes_) {
    ++skipped_too_large_;
    return;
  }

  auto existing = index_.find(key);
  if (existing != index_.end()) {
    bytes_used_ -= existing->second->size;
    lru_.erase(existing->second);
    index_.erase(existing);
  }

  EvictLocked(size);

  lru_.push_front(Node{key, std::move(value), size});
  index_[key] = lru_.begin();
  bytes_used_ += size;
  ++insertions_;
}

PackedAssetCache::Stats PackedAssetCache::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats stats;
  stats.hits = hits_;
  stats.misses = misses_;
  stats.evictions = evictions_;
  stats.insertions = insertions_;
  stats.skipped_too_large = skipped_too_large_;
  stats.bytes_used = bytes_used_;
  stats.capacity_bytes = capacity_bytes_;
  return stats;
}

}  // namespace flutter
