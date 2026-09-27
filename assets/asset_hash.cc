// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/asset_hash.h"

extern "C" uint64_t asset_hash_arm64(const uint8_t* data, size_t length) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

namespace flutter {

uint64_t AssetHashReference(const uint8_t* data, size_t length) {
  uint64_t h = 14695981039346656037ull;
  const uint8_t* p = data;
  const uint8_t* end = data + length;
  while (p != end) {
    h = (h ^ static_cast<uint64_t>(*p)) * 1099511628211ull;
    ++p;
  }
  return h;
}

}  // namespace flutter
