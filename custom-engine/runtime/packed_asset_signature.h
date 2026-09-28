// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_ASSETS_PACKED_ASSET_SIGNATURE_H_
#define FLUTTER_ASSETS_PACKED_ASSET_SIGNATURE_H_

#include <cstddef>
#include <cstdint>

namespace flutter {

class PackedAssetSignature {
 public:
  static constexpr size_t kPublicKeySize = 32;
  static constexpr size_t kPrivateKeySize = 64;
  static constexpr size_t kSignatureSize = 64;
  static constexpr size_t kDigestSize = 16;

  static void DigestBlock(const uint8_t* block,
                          size_t size,
                          uint8_t digest[kDigestSize]);

  static bool BlockMatches(const uint8_t* block,
                           size_t size,
                           const uint8_t digest[kDigestSize]);

  static bool SignIndex(const uint8_t* index,
                        size_t size,
                        const uint8_t private_key[kPrivateKeySize],
                        uint8_t signature[kSignatureSize]);

  static bool VerifyIndex(const uint8_t* index,
                          size_t size,
                          const uint8_t signature[kSignatureSize],
                          const uint8_t public_key[kPublicKeySize]);
};

}  // namespace flutter

#endif  // FLUTTER_ASSETS_PACKED_ASSET_SIGNATURE_H_
