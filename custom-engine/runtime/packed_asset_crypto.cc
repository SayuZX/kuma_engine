// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/assets/packed_asset_crypto.h"

#include <openssl/aead.h>

namespace flutter {

namespace {

const EVP_AEAD* Aead() {
  return EVP_aead_chacha20_poly1305();
}

}  // namespace

bool PackedAssetCrypto::Seal(const uint8_t key[kKeySize],
                             const uint8_t nonce[kNonceSize],
                             const uint8_t* plaintext,
                             size_t plaintext_len,
                             std::vector<uint8_t>* ciphertext_and_tag) {
  const EVP_AEAD* aead = Aead();
  EVP_AEAD_CTX* ctx =
      EVP_AEAD_CTX_new(aead, key, kKeySize, EVP_AEAD_DEFAULT_TAG_LENGTH);
  if (ctx == nullptr) {
    return false;
  }
  ciphertext_and_tag->resize(plaintext_len + EVP_AEAD_max_overhead(aead));
  size_t out_len = 0;
  const int ok = EVP_AEAD_CTX_seal(
      ctx, ciphertext_and_tag->data(), &out_len, ciphertext_and_tag->size(),
      nonce, kNonceSize, plaintext, plaintext_len, nullptr, 0);
  EVP_AEAD_CTX_free(ctx);
  if (ok != 1) {
    return false;
  }
  ciphertext_and_tag->resize(out_len);
  return true;
}

bool PackedAssetCrypto::Open(const uint8_t key[kKeySize],
                             const uint8_t nonce[kNonceSize],
                             const uint8_t* ciphertext_and_tag,
                             size_t ciphertext_and_tag_len,
                             std::vector<uint8_t>* plaintext) {
  const EVP_AEAD* aead = Aead();
  EVP_AEAD_CTX* ctx =
      EVP_AEAD_CTX_new(aead, key, kKeySize, EVP_AEAD_DEFAULT_TAG_LENGTH);
  if (ctx == nullptr) {
    return false;
  }
  plaintext->resize(ciphertext_and_tag_len);
  size_t out_len = 0;
  const int ok = EVP_AEAD_CTX_open(ctx, plaintext->data(), &out_len,
                                   plaintext->size(), nonce, kNonceSize,
                                   ciphertext_and_tag, ciphertext_and_tag_len,
                                   nullptr, 0);
  EVP_AEAD_CTX_free(ctx);
  if (ok != 1) {
    return false;
  }
  plaintext->resize(out_len);
  return true;
}

}  // namespace flutter
