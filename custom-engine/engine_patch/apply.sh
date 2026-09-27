#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_FLUTTER="${ENGINE_FLUTTER:-$(cd "$HERE/../.." && pwd)}"
RUNTIME="$HERE/../runtime"

if [[ ! -f "$ENGINE_FLUTTER/assets/asset_resolver.h" ]]; then
  echo "Expected Flutter engine source root at $ENGINE_FLUTTER" >&2
  exit 2
fi

if ! (cd "$ENGINE_FLUTTER" && git apply --check "$HERE/0001-packed-asset-resolver-integration.patch"); then
  echo "Patch does not apply cleanly; expected the kuma_engine 3.27 base." >&2
  exit 1
fi

mkdir -p "$ENGINE_FLUTTER/assets/testdata"
cp "$RUNTIME"/* "$ENGINE_FLUTTER/assets/"
cp "$HERE/../linker/testdata/payload.bin" \
  "$ENGINE_FLUTTER/assets/testdata/packed_payload_fixture.bin"
(cd "$ENGINE_FLUTTER" && git apply "$HERE/0001-packed-asset-resolver-integration.patch")

echo "Packed asset sources and engine integration applied to $ENGINE_FLUTTER"
