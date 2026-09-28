// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/packed_asset_signature.h"

#include <openssl/curve25519.h>
#include <openssl/mem.h>
#include <openssl/sha.h>

#include <cstring>

namespace flutter {

void PackedAssetSignature::DigestBlock(const uint8_t* block,
                                       size_t size,
                                       uint8_t digest[kDigestSize]) {
  uint8_t full[SHA256_DIGEST_LENGTH];
  SHA256(block, size, full);
  std::memcpy(digest, full, kDigestSize);
}

bool PackedAssetSignature::BlockMatches(const uint8_t* block,
                                        size_t size,
                                        const uint8_t digest[kDigestSize]) {
  uint8_t actual[kDigestSize];
  DigestBlock(block, size, actual);
  return CRYPTO_memcmp(actual, digest, kDigestSize) == 0;
}

bool PackedAssetSignature::SignIndex(const uint8_t* index,
                                     size_t size,
                                     const uint8_t private_key[kPrivateKeySize],
                                     uint8_t signature[kSignatureSize]) {
  return ED25519_sign(signature, index, size, private_key) == 1;
}

bool PackedAssetSignature::VerifyIndex(
    const uint8_t* index,
    size_t size,
    const uint8_t signature[kSignatureSize],
    const uint8_t public_key[kPublicKeySize]) {
  return ED25519_verify(index, size, signature, public_key) == 1;
}

}  // namespace flutter
