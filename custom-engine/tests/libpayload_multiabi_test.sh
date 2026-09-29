#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE_SRC="${ENGINE_SRC:-$ROOT/../engine/src}"
TOOLCHAIN="$ENGINE_SRC/flutter/buildtools/mac-x64/clang/bin"
PAYLOAD="$ROOT/linker/testdata/payload.bin"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for ABI in arm64-v8a armeabi-v7a; do
  OUT="$WORK/$ABI/libpayload.so"
  mkdir -p "$(dirname "$OUT")"
  if ! bash "$ROOT/linker/build_libpayload.sh" "$PAYLOAD" "$OUT" "$ABI" > "$WORK/$ABI.log" 2>&1; then
    cat "$WORK/$ABI.log" >&2
    exit 1
  fi
  MACHINE="$("$TOOLCHAIN/llvm-readelf" -h "$OUT" | sed -n 's/.*Machine:[[:space:]]*//p')"
  case "$ABI:$MACHINE" in
    arm64-v8a:AArch64|armeabi-v7a:ARM) ;;
    *) echo "wrong ELF machine for $ABI: $MACHINE" >&2; exit 1 ;;
  esac
  "$TOOLCHAIN/llvm-readelf" --dyn-syms "$OUT" | rg '__flutter_payload_start'
  "$TOOLCHAIN/llvm-readelf" --dyn-syms "$OUT" | rg '__flutter_payload_end'
  if "$TOOLCHAIN/llvm-readelf" -S "$OUT" | rg '\.symtab|\.debug_info'; then
    echo "unstripped native payload library: $ABI" >&2
    exit 1
  fi
done
