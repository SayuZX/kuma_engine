#!/usr/bin/env bash
set -euo pipefail

FLUTTER_ROOT="${FLUTTER_ROOT:-$HOME/development/flutter}"
ENGINE_SRC="${ENGINE_SRC:-$FLUTTER_ROOT/engine/src}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAYLOAD="${1:-$HERE/testdata/payload.bin}"

TC="$ENGINE_SRC/flutter/buildtools/mac-x64/clang/bin"
CLANG="$TC/clang"
OBJCOPY="$TC/llvm-objcopy"
READELF="$TC/llvm-readelf"
NM="$TC/llvm-nm"
NDK="$ENGINE_SRC/flutter/third_party/android_tools/sdk/ndk/28.2.13676358"
SYSROOT="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot"
TGT="aarch64-linux-android24"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SIZE="$(stat -f '%z' "$PAYLOAD")"

payload_segment() {
  "$READELF" -l "$1" 2>/dev/null | awk '
    /Section to Segment/{f=1; next}
    f && /flutter_payload/{
      seg=$1
      loadable=(seg ~ /^[0-9]+$/) ? "yes" : "no"
      print seg, loadable
    }'
}

echo "==> building stand-in appstub.o"
printf 'const unsigned char kDartIsolateSnapshotData[16]={0};\nint DartMain(){return 0;}\n' \
  > "$WORK/appstub.c"
"$CLANG" --target="$TGT" --sysroot="$SYSROOT" -nostdlib -c "$WORK/appstub.c" \
  -o "$WORK/appstub.o"
"$CLANG" --target="$TGT" --sysroot="$SYSROOT" -nostdlib -shared \
  -o "$WORK/libapp.base.so" "$WORK/appstub.o"

echo ""
echo "== method 1: objcopy --add-section (post-link) =="
"$OBJCOPY" --add-section .flutter_payload="$PAYLOAD" \
  --set-section-flags .flutter_payload=alloc,readonly,contents \
  "$WORK/libapp.base.so" "$WORK/libapp.objcopy.so"
"$OBJCOPY" --add-symbol __flutter_payload_start=.flutter_payload:0,global \
  --add-symbol __flutter_payload_end=.flutter_payload:"$SIZE",global \
  "$WORK/libapp.objcopy.so" "$WORK/libapp.objcopy.so"
read -r SEG LOAD <<<"$(payload_segment "$WORK/libapp.objcopy.so")"
echo "   .flutter_payload segment=$SEG loadable=$LOAD"
if [[ "$LOAD" == "yes" ]]; then
  echo "   unexpected: objcopy section became loadable"
else
  echo "   CONSTRAINT: post-link section is not in a PT_LOAD -> not mapped at"
  echo "   runtime; bracket symbols point outside the payload. objcopy alone is"
  echo "   insufficient for Backend B."
fi

echo ""
echo "== method 2: link-time embed (payload_section.o in the app link) =="
"$CLANG" --target="$TGT" --sysroot="$SYSROOT" \
  -DPAYLOAD_FILE="\"$PAYLOAD\"" -c "$HERE/payload_section.S" \
  -o "$WORK/payload_section.o"
"$CLANG" --target="$TGT" --sysroot="$SYSROOT" -nostdlib -shared \
  -Wl,--export-dynamic \
  -o "$WORK/libapp.linked.so" "$WORK/appstub.o" "$WORK/payload_section.o"
read -r SEG LOAD <<<"$(payload_segment "$WORK/libapp.linked.so")"
echo "   .flutter_payload segment=$SEG loadable=$LOAD"
START="$("$NM" "$WORK/libapp.linked.so" | awk '/__flutter_payload_start/{print $1}')"
END="$("$NM" "$WORK/libapp.linked.so" | awk '/__flutter_payload_end/{print $1}')"
SPAN="$(( 0x$END - 0x$START ))"
echo "   start=0x$START end=0x$END span=$SPAN (payload $SIZE)"
if [[ "$LOAD" == "yes" && "$SPAN" -eq "$SIZE" ]]; then
  echo "   OK: link-time embed places the payload in a loadable segment with"
  echo "   valid bracket symbols. This is the correct Backend B mechanism."
else
  echo "   FAIL link-time embed did not produce a loadable payload"
  exit 1
fi
