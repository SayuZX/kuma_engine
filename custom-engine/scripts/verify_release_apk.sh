#!/usr/bin/env bash
set -euo pipefail

APK="${1:-}"
if [[ -z "$APK" || ! -f "$APK" ]]; then
  echo "usage: verify_release_apk.sh <app-release.apk>"
  exit 2
fi

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

echo "==> [4] symbol / debug-info check on libflutter.so"
if [[ -f "$LIBFLUTTER" ]]; then
  if llvm-readelf -S "$LIBFLUTTER" 2>/dev/null | grep -qE '\.debug_info'; then
    fail "libflutter.so contains .debug_info (not stripped)"
  else
    pass "libflutter.so has no .debug_info"
  fi
else
  echo "warn libflutter.so not found; skipping symbol check"
fi

echo "==> [5] plaintext asset path check"
FOUND_PATHS=0
for so in "$LIBAPP" "$LIBFLUTTER"; do
  [[ -f "$so" ]] || continue
  if strings -a "$so" | grep -qE '(^|/)assets/[A-Za-z0-9_./-]+\.(png|jpg|jpeg|webp|json|bin|ttf|otf|mp3|mp4)'; then
    fail "plaintext asset paths found in $(basename "$so")"
    FOUND_PATHS=1
  fi
done
if [[ "$FOUND_PATHS" -eq 0 ]]; then
  pass "no plaintext asset file paths in native libraries"
fi

echo "==> [6] binary size report"
for so in "$LIBAPP" "$LIBFLUTTER"; do
  [[ -f "$so" ]] || continue
  echo "     $(basename "$so"): $(du -h "$so" | cut -f1)"
done

echo "==> [7] embedded payload section present in a native lib"
PAYLOAD_OK=0
for so in "$LIBAPP" "$LIBFLUTTER" "$WORK"/lib/arm64-v8a/libpayload.so; do
  [[ -f "$so" ]] || continue
  if llvm-readelf -S "$so" 2>/dev/null | grep -q '\.flutter_payload'; then
    pass "found .flutter_payload in $(basename "$so")"
    PAYLOAD_OK=1
  fi
done
if [[ "$PAYLOAD_OK" -eq 0 ]]; then
  echo "warn no .flutter_payload section found (expected once Backend A/B lands)"
fi

if [[ "$STATUS" -eq 0 ]]; then
  echo "VERIFY PASS"
else
  echo "VERIFY FAIL"
fi
exit "$STATUS"
