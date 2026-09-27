#!/usr/bin/env bash
set -euo pipefail

FLUTTER_ROOT="${FLUTTER_ROOT:-$HOME/development/flutter}"
ENGINE_SRC="${ENGINE_SRC:-$FLUTTER_ROOT/engine/src}"
HOST_OUT="${HOST_OUT:-out/host_debug_unopt}"
export PATH="$HOME/development/depot_tools:$PATH"

cd "$ENGINE_SRC"

echo "==> gn gen (host, unoptimized)"
./flutter/tools/gn --unoptimized >/dev/null

echo "==> build resolver unit tests + file checker + section test"
ninja -C "$HOST_OUT" packed_asset_resolver_smoketest \
  packed_asset_resolver_filecheck packed_payload_section_test

echo "==> run unit tests"
"./$HOST_OUT/packed_asset_resolver_smoketest"

echo "==> run embedded-section resolver test"
"./$HOST_OUT/packed_payload_section_test"

echo "==> run packer round-trip"
"$FLUTTER_ROOT/bin/cache/dart-sdk/bin/dart" \
  "$FLUTTER_ROOT/custom-engine/asset_packer/packer_test.dart"

echo "==> compression benchmark"
"$FLUTTER_ROOT/bin/cache/dart-sdk/bin/dart" \
  "$FLUTTER_ROOT/custom-engine/benchmarks/compression_bench.dart"

echo "==> run packer -> engine resolver integration"
bash "$FLUTTER_ROOT/custom-engine/tests/integration_roundtrip.sh"
