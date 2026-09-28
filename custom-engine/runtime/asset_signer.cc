// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <openssl/curve25519.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "flutter/assets/packed_asset_resolver.h"
#include "flutter/assets/packed_asset_signature.h"

namespace {

uint16_t U16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t U32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t U64(const uint8_t* p) {
  return static_cast<uint64_t>(U32(p)) |
         (static_cast<uint64_t>(U32(p + 4)) << 32);
}

void Put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void Put32(uint8_t* p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

void Put64(uint8_t* p, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

bool Fits(uint64_t offset, uint64_t size, size_t total) {
  return offset <= total && size <= total - offset;
}

bool ReadFile(const char* path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    return false;
  }
  if (std::fseek(f, 0, SEEK_END) != 0) {
    std::fclose(f);
    return false;
  }
  const long length = std::ftell(f);
  if (length < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(length));
  const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

bool WriteFile(const char* path,
               const uint8_t* data,
               size_t size,
               bool private_file) {
  if (private_file) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      return false;
    }
    size_t written = 0;
    while (written < size) {
      const ssize_t count = write(fd, data + written, size - written);
      if (count <= 0) {
        close(fd);
        return false;
      }
      written += static_cast<size_t>(count);
    }
    return close(fd) == 0;
  }
  FILE* f = std::fopen(path, "wb");
  if (!f) {
    return false;
  }
  const bool ok = std::fwrite(data, 1, size, f) == size;
  return std::fclose(f) == 0 && ok;
}

bool SignPayload(const std::vector<uint8_t>& input,
                 const uint8_t private_key[64],
                 size_t alignment,
                 std::vector<uint8_t>* output) {
  if (input.size() < flutter::PackedAssetResolver::kHeaderSize ||
      std::memcmp(input.data(), "FEAP", 4) != 0 || U16(input.data() + 4) != 2 ||
      U32(input.data() + 12) != flutter::PackedAssetResolver::kHeaderSize ||
      (U16(input.data() + 6) &
       ~static_cast<uint16_t>(
           flutter::PackedAssetResolver::kHeaderFlagEntryCrc32 |
           flutter::PackedAssetResolver::kHeaderFlagEncrypted)) != 0 ||
      U64(input.data() + 24) != 0) {
    return false;
  }
  flutter::PackedAssetResolver source(input.data(), input.size());
  if (!source.IsValid() || !source.VerifyIntegrity()) {
    return false;
  }
  const uint32_t count = U32(input.data() + 8);
  const uint64_t index_end =
      flutter::PackedAssetResolver::kHeaderSize +
      static_cast<uint64_t>(count) * flutter::PackedAssetResolver::kEntrySize;
  const uint64_t old_blob = U64(input.data() + 16);
  if (!Fits(flutter::PackedAssetResolver::kHeaderSize,
            index_end - flutter::PackedAssetResolver::kHeaderSize,
            input.size()) ||
      old_blob < index_end || old_blob > input.size()) {
    return false;
  }
  const uint64_t digest_bytes =
      static_cast<uint64_t>(count) * flutter::PackedAssetSignature::kDigestSize;
  const uint64_t signature_offset = index_end + digest_bytes;
  const uint64_t signed_end =
      signature_offset + flutter::PackedAssetSignature::kSignatureSize;
  if (signed_end > std::numeric_limits<size_t>::max() - (alignment - 1)) {
    return false;
  }
  const size_t new_blob =
      (static_cast<size_t>(signed_end) + alignment - 1) & ~(alignment - 1);
  const size_t blob_bytes = input.size() - static_cast<size_t>(old_blob);
  if (blob_bytes > std::numeric_limits<size_t>::max() - new_blob) {
    return false;
  }

  output->assign(new_blob + blob_bytes, 0);
  std::memcpy(output->data(), input.data(), static_cast<size_t>(index_end));
  Put16(output->data() + 4, flutter::PackedAssetResolver::kSignedFormatVersion);
  const uint16_t flags = U16(input.data() + 6);
  Put16(output->data() + 6,
        (flags & ~flutter::PackedAssetResolver::kHeaderFlagEntryCrc32) |
            flutter::PackedAssetResolver::kHeaderFlagSignedIndex);
  Put64(output->data() + 16, new_blob);
  Put64(output->data() + 24, index_end);

  uint64_t previous_hash = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* row =
        output->data() + flutter::PackedAssetResolver::kHeaderSize +
        static_cast<size_t>(i) * flutter::PackedAssetResolver::kEntrySize;
    const uint64_t hash = U64(row);
    if (i != 0 && hash <= previous_hash) {
      return false;
    }
    previous_hash = hash;
    const uint64_t offset = U64(row + 8);
    const uint32_t stored = U32(row + 16);
    if (!Fits(offset, stored, blob_bytes)) {
      return false;
    }
    flutter::PackedAssetSignature::DigestBlock(
        input.data() + old_blob + offset, stored,
        output->data() + index_end +
            static_cast<size_t>(i) *
                flutter::PackedAssetSignature::kDigestSize);
    Put32(row + 28, 0);
  }
  if (!flutter::PackedAssetSignature::SignIndex(
          output->data(), static_cast<size_t>(signature_offset), private_key,
          output->data() + signature_offset)) {
    return false;
  }
  std::memcpy(output->data() + new_blob, input.data() + old_blob, blob_bytes);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const char* action = argc > 1 ? argv[1] : "";
  const char* input = nullptr;
  const char* output = nullptr;
  const char* private_path = nullptr;
  const char* public_path = nullptr;
  bool index_only = false;
  size_t alignment = 16;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--index-only") {
      index_only = true;
      continue;
    }
    if (i + 1 >= argc) {
      return 2;
    }
    const char* value = argv[++i];
    if (arg == "--input") {
      input = value;
    } else if (arg == "--output") {
      output = value;
    } else if (arg == "--private") {
      private_path = value;
    } else if (arg == "--public") {
      public_path = value;
    } else if (arg == "--alignment") {
      const unsigned long parsed = std::strtoul(value, nullptr, 10);
      alignment = static_cast<size_t>(parsed);
    } else {
      return 2;
    }
  }
  if (std::strcmp(action, "keygen") == 0 && !index_only && private_path &&
      public_path) {
    std::array<uint8_t, 32> public_key{};
    std::array<uint8_t, 64> private_key{};
    ED25519_keypair(public_key.data(), private_key.data());
    if (!WriteFile(private_path, private_key.data(), private_key.size(),
                   true) ||
        !WriteFile(public_path, public_key.data(), public_key.size(), false)) {
      std::fprintf(stderr, "failed writing key files\n");
      return 1;
    }
    return 0;
  }
  if (std::strcmp(action, "export-header") == 0 && !index_only && public_path &&
      output) {
    std::vector<uint8_t> public_key;
    if (!ReadFile(public_path, &public_key) || public_key.size() != 32) {
      return 1;
    }
    std::string header =
        "#ifndef FLUTTER_ASSETS_PACKED_ASSET_PUBLIC_KEY_GENERATED_H_\n"
        "#define FLUTTER_ASSETS_PACKED_ASSET_PUBLIC_KEY_GENERATED_H_\n"
        "#include <cstdint>\nnamespace flutter {\n"
        "constexpr uint8_t kPackedPayloadPublicKey[32] = {";
    for (size_t i = 0; i < public_key.size(); ++i) {
      char byte[8];
      std::snprintf(byte, sizeof(byte), "0x%02x%s", public_key[i],
                    i + 1 == public_key.size() ? "" : ",");
      header += byte;
    }
    header += "};\n}\n#endif\n";
    return WriteFile(output, reinterpret_cast<const uint8_t*>(header.data()),
                     header.size(), false)
               ? 0
               : 1;
  }
  if (std::strcmp(action, "verify") == 0 && input && public_path) {
    std::vector<uint8_t> payload;
    std::vector<uint8_t> public_key;
    if (!ReadFile(input, &payload) || !ReadFile(public_path, &public_key) ||
        public_key.size() != flutter::PackedAssetSignature::kPublicKeySize) {
      return 1;
    }
    flutter::PackedAssetResolver resolver(payload.data(), payload.size(),
                                          nullptr, nullptr, public_key.data());
    if (!resolver.IsValid() || (!index_only && !resolver.VerifyIntegrity())) {
      std::fprintf(stderr, "signature or block digest verification failed\n");
      return 1;
    }
    std::fprintf(stdout, index_only ? "signed index verified\n"
                                    : "signed payload verified\n");
    return 0;
  }
  if (std::strcmp(action, "sign") == 0 && !index_only && input && output &&
      private_path && alignment >= 1 && alignment <= 4096 &&
      (alignment & (alignment - 1)) == 0) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> private_key;
    std::vector<uint8_t> signed_bytes;
    if (!ReadFile(input, &bytes) || !ReadFile(private_path, &private_key) ||
        private_key.size() != 64 ||
        !SignPayload(bytes, private_key.data(), alignment, &signed_bytes) ||
        !WriteFile(output, signed_bytes.data(), signed_bytes.size(), false)) {
      std::fprintf(stderr, "failed signing payload\n");
      return 1;
    }
    return 0;
  }
  std::fprintf(stderr,
               "usage: asset_signer keygen --private <file> --public <file>\n"
               "       asset_signer sign --input <v2.bin> --output <v3.bin> "
               "--private <file> [--alignment 16]\n"
               "       asset_signer export-header --public <file> --output "
               "<generated.h>\n"
               "       asset_signer verify --input <v3.bin> --public <file> "
               "[--index-only]\n");
  return 2;
}
