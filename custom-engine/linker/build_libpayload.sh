#!/usr/bin/env bash
set -euo pipefail

FLUTTER_ROOT="${FLUTTER_ROOT:-$HOME/development/flutter}"
ENGINE_SRC="${ENGINE_SRC:-$FLUTTER_ROOT/engine/src}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PAYLOAD="${1:-$HERE/testdata/payload.bin}"
OUT="${2:-$HERE/testdata/libpayload.so}"

TOOLCHAIN="$ENGINE_SRC/flutter/buildtools/mac-x64/clang/bin"
CLANG="$TOOLCHAIN/clang"
READELF="$TOOLCHAIN/llvm-readelf"
NM="$TOOLCHAIN/llvm-nm"
NDK="$ENGINE_SRC/flutter/third_party/android_tools/sdk/ndk/28.2.13676358"
SYSROOT="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot"
TARGET="aarch64-linux-android24"

if [[ ! -f "$PAYLOAD" ]]; then
  echo "payload not found: $PAYLOAD"
  exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> assembling payload_section.S (arm64 ELF)"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" \
  -DPAYLOAD_FILE="\"$PAYLOAD\"" \
  -c "$HERE/payload_section.S" -o "$WORK/payload.o"

echo "==> linking libpayload.so"
"$CLANG" --target="$TARGET" --sysroot="$SYSROOT" -shared -nostdlib \
  -Wl,-soname,libpayload.so \
  -Wl,--version-script,"$HERE/libpayload.ver" \
  -Wl,--gc-sections \
  -o "$OUT" "$WORK/payload.o"

PAYLOAD_SIZE="$(stat -f '%z' "$PAYLOAD")"

echo "==> [1] section .flutter_payload present and allocatable"
"$READELF" -S "$OUT" | grep -A1 "flutter_payload" || { echo "FAIL no section"; exit 1; }

echo "==> [2] exported symbols"
"$READELF" --dyn-syms "$OUT" | grep "__flutter_payload_" || { echo "FAIL no symbols"; exit 1; }

echo "==> [3] end - start == payload size ($PAYLOAD_SIZE)"
START="$("$NM" -D "$OUT" | awk '/__flutter_payload_start/{print $1}')"
END="$("$NM" -D "$OUT" | awk '/__flutter_payload_end/{print $1}')"
SPAN="$(( 0x$END - 0x$START ))"
echo "     start=0x$START end=0x$END span=$SPAN"
if [[ "$SPAN" -eq "$PAYLOAD_SIZE" ]]; then
  echo "ok   span matches payload size"
else
  echo "FAIL span $SPAN != payload size $PAYLOAD_SIZE"
  exit 1
fi

echo "==> [4] machine + file type"
"$READELF" -h "$OUT" | grep -E "Machine|Type:"

echo "LIBPAYLOAD OK -> $OUT ($(stat -f '%z' "$OUT") bytes)"
