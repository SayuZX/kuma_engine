// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_ASSETS_ASSET_HASH_H_
#define FLUTTER_ASSETS_ASSET_HASH_H_

#include <cstddef>
#include <cstdint>

extern "C" uint64_t asset_hash_arm64(const uint8_t* data, size_t length);

namespace flutter {

uint64_t AssetHashReference(const uint8_t* data, size_t length);

}  // namespace flutter

#endif  // FLUTTER_ASSETS_ASSET_HASH_H_
