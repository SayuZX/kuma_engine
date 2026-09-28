#!/usr/bin/env bash
set -euo pipefail

ENGINE_ROOT="${ENGINE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
ENGINE_SRC="${ENGINE_SRC:-$(dirname "$ENGINE_ROOT")}"
HOST_OUT="${HOST_OUT:-out/host_debug_unopt}"
DART="${DART:-dart}"

if [[ ! -f "$ENGINE_SRC/flutter/tools/gn" ]]; then
  echo "ENGINE_SRC must point to a synced engine workspace containing flutter/tools/gn" >&2
  exit 2
fi

cd "$ENGINE_SRC"

echo "==> gn gen (host, unoptimized)"
./flutter/tools/gn --unoptimized >/dev/null

echo "==> build resolver unit tests + file checker + section test"
ninja -C "$HOST_OUT" packed_asset_resolver_smoketest \
  packed_asset_resolver_filecheck packed_payload_section_test asset_signer

echo "==> run unit tests"
"./$HOST_OUT/packed_asset_resolver_smoketest"

echo "==> run embedded-section resolver test"
"./$HOST_OUT/packed_payload_section_test"

echo "==> run packer round-trip"
"$DART" "$ENGINE_ROOT/custom-engine/asset_packer/packer_test.dart"

echo "==> compression benchmark"
"$DART" "$ENGINE_ROOT/custom-engine/benchmarks/compression_bench.dart"

echo "==> run packer -> engine resolver integration"
bash "$ENGINE_ROOT/custom-engine/tests/integration_roundtrip.sh"

echo "==> run synthetic APK release gate tests"
ENGINE_SRC="$ENGINE_SRC" python3 \
  "$ENGINE_ROOT/custom-engine/tests/release_gate_fixture.py"
