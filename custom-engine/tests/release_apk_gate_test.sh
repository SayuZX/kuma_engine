#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

python3 - "$WORK/unsigned.apk" "$ROOT/linker/testdata/payload.bin" "$WORK/public.ed25519" <<'PY'
import sys
from pathlib import Path
from zipfile import ZIP_STORED, ZipFile

Path(sys.argv[3]).write_bytes(bytes(32))
with ZipFile(sys.argv[1], 'w') as archive:
    archive.writestr('AndroidManifest.xml', b'not a real manifest', ZIP_STORED)
    archive.writestr('classes.dex', b'fixture', ZIP_STORED)
    archive.writestr('resources.arsc', b'fixture', ZIP_STORED)
    archive.writestr('lib/arm64-v8a/libapp.so', b'app', ZIP_STORED)
    archive.writestr('lib/arm64-v8a/libflutter.so', b'engine', ZIP_STORED)
    archive.write(sys.argv[2], 'lib/arm64-v8a/libpayload.so', compress_type=ZIP_STORED)
PY

if python3 "$ROOT/scripts/verify_release_apk.py" "$WORK/unsigned.apk" \
    --public-key "$WORK/public.ed25519" \
    --engine-src "${ENGINE_SRC:-$ROOT/../engine/src}" \
    --android-build-tools "${ANDROID_BUILD_TOOLS:-/usr/local/share/android-commandlinetools/build-tools/35.0.0}" \
    > "$WORK/result.log" 2>&1; then
  cat "$WORK/result.log" >&2
  echo "unsigned APK incorrectly passed the release gate" >&2
  exit 1
fi
grep -q 'apksigner' "$WORK/result.log" || {
  cat "$WORK/result.log" >&2
  echo "release gate did not reach APK signature verification" >&2
  exit 1
}
