#!/usr/bin/env bash
set -euo pipefail

APK="${1:-}"
PUBLIC_KEY=""
if [[ $# -eq 3 && "${2:-}" == "--public" ]]; then
  PUBLIC_KEY="$3"
else
  echo "usage: verify_release_apk.sh <app-release.apk> --public <ed25519-public-key>"
  exit 2
fi
if [[ ! -f "$APK" || ! -f "$PUBLIC_KEY" ]]; then
  echo "usage: verify_release_apk.sh <app-release.apk> --public <ed25519-public-key>"
  exit 2
fi

LLVM_READELF="${LLVM_READELF:-llvm-readelf}"
LLVM_OBJCOPY="${LLVM_OBJCOPY:-llvm-objcopy}"
ASSET_SIGNER="${ASSET_SIGNER:-asset_signer}"
for tool in "$LLVM_READELF" "$LLVM_OBJCOPY" "$ASSET_SIGNER" unzip strings; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required tool not found: $tool" >&2
    exit 2
  fi
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STATUS=0

fail() {
  echo "FAIL $1"
  STATUS=1
}

pass() {
  echo "ok   $1"
}

echo "==> listing $APK"
unzip -l "$APK" > "$WORK/list.txt"

echo "==> [1] flutter_assets/ must be absent"
if grep -q "assets/flutter_assets/" "$WORK/list.txt"; then
  fail "assets/flutter_assets/ is present in the APK"
else
  pass "no assets/flutter_assets/ directory"
fi

echo "==> [2] ABI must be arm64-v8a only"
LIB_ABIS="$(grep -oE 'lib/[^/]+/' "$WORK/list.txt" | sort -u || true)"
if [[ -z "$LIB_ABIS" ]]; then
  fail "no native libraries found under lib/"
elif [[ "$LIB_ABIS" != "lib/arm64-v8a/" ]]; then
  fail "unexpected ABIs: $(echo "$LIB_ABIS" | tr '\n' ' ')"
else
  pass "only lib/arm64-v8a/ present"
fi

echo "==> [3] extracting native libraries"
unzip -o -q "$APK" 'lib/arm64-v8a/*' -d "$WORK" || true
LIBAPP="$WORK/lib/arm64-v8a/libapp.so"
LIBFLUTTER="$WORK/lib/arm64-v8a/libflutter.so"
LIBPAYLOAD="$WORK/lib/arm64-v8a/libpayload.so"
if [[ ! -f "$LIBAPP" ]]; then
  fail "libapp.so not found"
fi
for so in "$LIBAPP" "$LIBFLUTTER" "$LIBPAYLOAD"; do
  [[ -f "$so" ]] || continue
  if ! elf_header="$("$LLVM_READELF" -h "$so" 2>/dev/null)" ||
      [[ "$elf_header" != *AArch64* ]] || [[ "$elf_header" != *DYN* ]]; then
    fail "$(basename "$so") is not an AArch64 shared object"
  fi
done

echo "==> [4] symbol / debug-info check on libflutter.so"
if [[ -f "$LIBFLUTTER" ]]; then
  if "$LLVM_READELF" -S "$LIBFLUTTER" 2>/dev/null | grep -E '\.debug_info|\.symtab' >/dev/null; then
    fail "libflutter.so contains debug info or a static symbol table"
  else
    pass "libflutter.so has no debug info or static symbol table"
  fi
else
  fail "libflutter.so not found"
fi

echo "==> [5] plaintext asset path check"
FOUND_PATHS=0
for so in "$LIBAPP" "$LIBFLUTTER" "$LIBPAYLOAD"; do
  [[ -f "$so" ]] || continue
  if strings -a "$so" | grep -E '(^|/)assets/[A-Za-z0-9_./-]+\.(png|jpg|jpeg|webp|json|bin|ttf|otf|mp3|mp4)' >/dev/null; then
    fail "plaintext asset paths found in $(basename "$so")"
    FOUND_PATHS=1
  fi
done
if [[ "$FOUND_PATHS" -eq 0 ]]; then
  pass "no plaintext asset file paths in native libraries"
fi

echo "==> [6] binary size report"
for so in "$LIBAPP" "$LIBFLUTTER" "$LIBPAYLOAD"; do
  [[ -f "$so" ]] || continue
  echo "     $(basename "$so"): $(du -h "$so" | cut -f1)"
done

echo "==> [7] embedded payload section present in a native lib"
PAYLOAD_OK=0
for so in "$LIBAPP" "$LIBFLUTTER" "$LIBPAYLOAD"; do
  [[ -f "$so" ]] || continue
  if "$LLVM_READELF" -S "$so" 2>/dev/null | grep '\.flutter_payload' >/dev/null; then
    pass "found .flutter_payload in $(basename "$so")"
    PAYLOAD_OK=$((PAYLOAD_OK + 1))
    if ! "$LLVM_READELF" -l "$so" 2>/dev/null | grep '\.flutter_payload' >/dev/null; then
      fail ".flutter_payload is not mapped into a loadable segment in $(basename "$so")"
    fi
    dumped="$WORK/$(basename "$so").payload.bin"
    if ! "$LLVM_OBJCOPY" --dump-section ".flutter_payload=$dumped" "$so" \
        >/dev/null 2>&1 || [[ ! -s "$dumped" ]]; then
      fail "cannot extract .flutter_payload from $(basename "$so")"
    elif ! "$ASSET_SIGNER" verify --input "$dumped" --public "$PUBLIC_KEY" \
        >/dev/null 2>&1; then
      fail "payload signature or block digest invalid in $(basename "$so")"
    else
      pass "payload signature and block digests valid in $(basename "$so")"
    fi
  fi
done
if [[ "$PAYLOAD_OK" -eq 0 ]]; then
  fail "no .flutter_payload section found"
elif [[ "$PAYLOAD_OK" -ne 1 ]]; then
  fail "expected one .flutter_payload section; found $PAYLOAD_OK"
fi

if [[ "$STATUS" -eq 0 ]]; then
  echo "VERIFY PASS"
else
  echo "VERIFY FAIL"
fi
exit "$STATUS"
