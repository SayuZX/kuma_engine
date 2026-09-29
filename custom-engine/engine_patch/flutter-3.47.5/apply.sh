#!/usr/bin/env bash
set -euo pipefail

FLUTTER_ROOT="${1:?usage: apply.sh <clean Flutter 3.47.5 checkout>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
PINNED_COMMIT=6a19cca56475dbfba1478ee68d7bd0c2ef891da1
ACTUAL_COMMIT="$(git -C "$FLUTTER_ROOT" rev-parse HEAD)"
if [[ "$ACTUAL_COMMIT" != "$PINNED_COMMIT" ]]; then
  echo "Expected Flutter commit $PINNED_COMMIT; found $ACTUAL_COMMIT" >&2
  exit 2
fi

TARGET="$FLUTTER_ROOT/engine/src/flutter/assets"
SOURCES=(
  asset_encryptor.cc
  asset_hash.cc asset_hash.h asset_hash_unittests.cc
  asset_signer.cc
  packed_asset_cache.cc packed_asset_cache.h packed_asset_cache_unittests.cc
  packed_asset_crypto.cc packed_asset_crypto.h
  packed_asset_resolver.cc packed_asset_resolver.h
  packed_asset_resolver_filecheck.cc packed_asset_resolver_unittests.cc
  packed_asset_signature.cc packed_asset_signature.h
  packed_payload_section.S packed_payload_section_test.cc
)

git -C "$FLUTTER_ROOT" apply --check "$HERE/integration.patch"
for source in "${SOURCES[@]}"; do
  origin="$REPO_ROOT/assets/$source"
  if [[ "$source" == packed_asset_signature.cc ]]; then
    origin="$HERE/$source"
  fi
  [[ -f "$origin" ]] || { echo "Missing source: $origin" >&2; exit 2; }
  [[ ! -e "$TARGET/$source" ]] || {
    echo "Refusing to replace existing source: $TARGET/$source" >&2
    exit 2
  }
done
[[ ! -e "$TARGET/packed_asset_public_key_generated.h" ]] || {
  echo "Refusing to replace an existing public-key header" >&2
  exit 2
}
[[ ! -e "$TARGET/testdata/packed_payload_fixture.bin" ]] || {
  echo "Refusing to replace an existing test fixture" >&2
  exit 2
}

git -C "$FLUTTER_ROOT" apply "$HERE/integration.patch"
for source in "${SOURCES[@]}"; do
  origin="$REPO_ROOT/assets/$source"
  if [[ "$source" == packed_asset_signature.cc ]]; then
    origin="$HERE/$source"
  fi
  cp "$origin" "$TARGET/$source"
done
mkdir -p "$TARGET/testdata"
cp "$REPO_ROOT/assets/testdata/packed_payload_fixture.bin" \
  "$TARGET/testdata/packed_payload_fixture.bin"

python3 - "$REPO_ROOT/custom-engine/keys/payload-public.ed25519" \
  "$TARGET/packed_asset_public_key_generated.h" <<'PY'
from pathlib import Path
import sys

key = Path(sys.argv[1]).read_bytes()
if len(key) != 32:
    raise SystemExit("Ed25519 public key must be exactly 32 bytes")
values = ",".join(f"0x{byte:02x}" for byte in key)
Path(sys.argv[2]).write_text(
    "#ifndef FLUTTER_ASSETS_PACKED_ASSET_PUBLIC_KEY_GENERATED_H_\n"
    "#define FLUTTER_ASSETS_PACKED_ASSET_PUBLIC_KEY_GENERATED_H_\n"
    "#include <cstdint>\n"
    "namespace flutter {\n"
    f"constexpr uint8_t kPackedPayloadPublicKey[32] = {{{values}}};\n"
    "}\n"
    "#endif\n",
    encoding="utf-8",
)
PY

echo "Flutter 3.47.5 packed-asset engine patch applied to $FLUTTER_ROOT"
