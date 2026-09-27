// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_ASSETS_PACKED_ASSET_CACHE_H_
#define FLUTTER_ASSETS_PACKED_ASSET_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "flutter/fml/macros.h"

namespace flutter {

class PackedAssetCache {
 public:
  using Buffer = std::shared_ptr<const std::vector<uint8_t>>;

  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t evictions = 0;
    uint64_t insertions = 0;
    uint64_t skipped_too_large = 0;
    size_t bytes_used = 0;
    size_t capacity_bytes = 0;
  };

  PackedAssetCache(size_t capacity_bytes, size_t max_item_bytes);

  Buffer Get(uint64_t key);

  void Put(uint64_t key, Buffer value);

  Stats GetStats() const;

 private:
  struct Node {
    uint64_t key;
    Buffer value;
    size_t size;
  };

  void EvictLocked(size_t incoming);

  const size_t capacity_bytes_;
  const size_t max_item_bytes_;

  mutable std::mutex mutex_;
  std::list<Node> lru_;
  std::unordered_map<uint64_t, std::list<Node>::iterator> index_;
  size_t bytes_used_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t evictions_ = 0;
  uint64_t insertions_ = 0;
  uint64_t skipped_too_large_ = 0;

  FML_DISALLOW_COPY_AND_ASSIGN(PackedAssetCache);
};

}  // namespace flutter

#endif  // FLUTTER_ASSETS_PACKED_ASSET_CACHE_H_
