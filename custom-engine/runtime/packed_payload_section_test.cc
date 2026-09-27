// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "flutter/assets/packed_asset_resolver.h"
#include "gtest/gtest.h"

extern "C" const uint8_t __flutter_payload_start[];
extern "C" const uint8_t __flutter_payload_end[];

namespace flutter {
namespace testing {

namespace {

std::unique_ptr<PackedAssetResolver> ResolverFromSection() {
  const uint8_t* start = __flutter_payload_start;
  const uint8_t* end = __flutter_payload_end;
  if (start == nullptr || end == nullptr || end <= start) {
    return nullptr;
  }
  return std::make_unique<PackedAssetResolver>(
      start, static_cast<size_t>(end - start));
}

std::string Read(const PackedAssetResolver& resolver, const std::string& key) {
  auto mapping = resolver.GetAsMapping(key);
  if (mapping == nullptr) {
    return "<null>";
  }
  return std::string(reinterpret_cast<const char*>(mapping->GetMapping()),
                     mapping->GetSize());
}

}  // namespace

TEST(PackedPayloadSectionTest, ResolverReadsAssetsFromEmbeddedSection) {
  auto resolver = ResolverFromSection();
  ASSERT_NE(resolver, nullptr);
  ASSERT_TRUE(resolver->IsValid());

  EXPECT_EQ(Read(*resolver, "assets/hello.txt"), "Hello from packed payload!\n");
  EXPECT_EQ(Read(*resolver, "assets/data/config.json"),
            "{\"env\":\"prod\",\"n\":42}");
  EXPECT_EQ(Read(*resolver, "FontManifest.json"), "FONTMANIFEST-BYTES");
  EXPECT_EQ(resolver->GetAsMapping("assets/not_here.txt"), nullptr);
}

}  // namespace testing
}  // namespace flutter
