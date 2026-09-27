// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_ASSETS_PACKED_ASSET_CRYPTO_H_
#define FLUTTER_ASSETS_PACKED_ASSET_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace flutter {

class PackedAssetCrypto {
 public:
  static constexpr size_t kKeySize = 32;
  static constexpr size_t kNonceSize = 12;
  static constexpr size_t kTagSize = 16;

  static bool Seal(const uint8_t key[kKeySize],
                   const uint8_t nonce[kNonceSize],
                   const uint8_t* plaintext,
                   size_t plaintext_len,
                   std::vector<uint8_t>* ciphertext_and_tag);

  static bool Open(const uint8_t key[kKeySize],
                   const uint8_t nonce[kNonceSize],
                   const uint8_t* ciphertext_and_tag,
                   size_t ciphertext_and_tag_len,
                   std::vector<uint8_t>* plaintext);
};

}  // namespace flutter

#endif  // FLUTTER_ASSETS_PACKED_ASSET_CRYPTO_H_
