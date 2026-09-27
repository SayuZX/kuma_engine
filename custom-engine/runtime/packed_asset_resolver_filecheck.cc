// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <cstring>
#include <string>

#include "flutter/assets/packed_asset_resolver.h"
#include "flutter/fml/mapping.h"

namespace {

bool CompareAsset(const flutter::PackedAssetResolver& resolver,
                  const std::string& key,
                  const std::string& expected_path) {
  auto expected = fml::FileMapping::CreateReadOnly(expected_path);
  if (expected == nullptr) {
    std::fprintf(stderr, "FAIL cannot read expected file %s\n",
                 expected_path.c_str());
    return false;
  }
  auto actual = resolver.GetAsMapping(key);
  if (actual == nullptr) {
    std::fprintf(stderr, "FAIL resolver returned null for %s\n", key.c_str());
    return false;
  }
  if (actual->GetSize() != expected->GetSize() ||
      std::memcmp(actual->GetMapping(), expected->GetMapping(),
                  expected->GetSize()) != 0) {
    std::fprintf(stderr, "FAIL byte mismatch for %s\n", key.c_str());
    return false;
  }
  std::fprintf(stdout, "ok   %s (%zu bytes)\n", key.c_str(),
               actual->GetSize());
  return true;
}

}  // namespace

bool HexToKey(const char* hex, uint8_t key[32]) {
  if (std::strlen(hex) != 64) {
    return false;
  }
  for (int i = 0; i < 32; ++i) {
    unsigned int byte = 0;
    if (std::sscanf(hex + i * 2, "%2x", &byte) != 1) {
      return false;
    }
    key[i] = static_cast<uint8_t>(byte);
  }
  return true;
}

int main(int argc, char** argv) {
  int arg = 1;
  uint8_t key[32];
  const uint8_t* key_ptr = nullptr;
  if (argc >= 3 && std::strcmp(argv[1], "--key") == 0) {
    if (!HexToKey(argv[2], key)) {
      std::fprintf(stderr, "FAIL invalid --key\n");
      return 2;
    }
    key_ptr = key;
    arg = 3;
  }

  if (argc - arg < 3 || ((argc - arg) % 2) != 1) {
    std::fprintf(stderr,
                 "usage: %s [--key <64hex>] <payload.bin> <key> <expected> "
                 "[<key> <expected> ...]\n",
                 argv[0]);
    return 2;
  }

  auto payload = fml::FileMapping::CreateReadOnly(argv[arg]);
  if (payload == nullptr) {
    std::fprintf(stderr, "FAIL cannot read payload %s\n", argv[arg]);
    return 2;
  }

  flutter::PackedAssetResolver resolver(payload->GetMapping(),
                                        payload->GetSize(), nullptr, key_ptr);
  if (!resolver.IsValid()) {
    std::fprintf(stderr, "FAIL payload is not a valid FEAP payload\n");
    return 1;
  }

  bool ok = true;
  for (int i = arg + 1; i + 1 < argc; i += 2) {
    ok &= CompareAsset(resolver, argv[i], argv[i + 1]);
  }

  if (resolver.GetAsMapping("this/key/definitely/does/not/exist") != nullptr) {
    std::fprintf(stderr, "FAIL missing key did not return null\n");
    ok = false;
  }

  std::fprintf(stdout, "%s\n", ok ? "ALL PASS" : "FAILURE");
  return ok ? 0 : 1;
}
