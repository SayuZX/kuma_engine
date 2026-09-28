#!/usr/bin/env bash
set -euo pipefail

APP_DIR="${1:?usage: build_app.sh <app-directory> [A|B]}"
BACKEND="${2:-A}"
[[ "$BACKEND" == A || "$BACKEND" == B ]] || { echo 'backend must be A or B' >&2; exit 2; }
: "${ENGINE_SRC:?Set ENGINE_SRC}"
: "${FLUTTER_ROOT:?Set FLUTTER_ROOT}"
: "${ASSET_PRIVATE_KEY:?Set ASSET_PRIVATE_KEY}"
: "${ASSET_PUBLIC_KEY:?Set ASSET_PUBLIC_KEY}"
: "${APK_KEYSTORE:?Set APK_KEYSTORE; use a test keystore only for a demo}"
: "${APK_KEY_ALIAS:?Set APK_KEY_ALIAS}"
: "${APK_KS_PASSWORD:?Set APK_KS_PASSWORD}"
: "${APK_KEY_PASSWORD:?Set APK_KEY_PASSWORD}"
: "${ANDROID_BUILD_TOOLS:?Set ANDROID_BUILD_TOOLS to the pinned SDK build-tools directory}"
export APK_KS_PASSWORD APK_KEY_PASSWORD
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="$(cd "$APP_DIR" && pwd)"
ENGINE_OUT="${ENGINE_OUT:-android_release_arm64}"
HOST_OUT="${HOST_OUT:-host_debug_unopt}"
DART="${DART:-$FLUTTER_ROOT/bin/cache/dart-sdk/bin/dart}"
export LLVM_READELF="${LLVM_READELF:-$ENGINE_SRC/flutter/buildtools/mac-x64/clang/bin/llvm-readelf}"
export LLVM_OBJCOPY="${LLVM_OBJCOPY:-$ENGINE_SRC/flutter/buildtools/mac-x64/clang/bin/llvm-objcopy}"
export ASSET_SIGNER="${ASSET_SIGNER:-$ENGINE_SRC/out/$HOST_OUT/asset_signer}"

# Gradle wrapper parses quoted JVM options; these system properties override
# project defaults without editing the application's gradle.properties.
export GRADLE_OPTS="${GRADLE_OPTS:-} -Dorg.gradle.workers.max=1 -Dorg.gradle.parallel=false -Dorg.gradle.daemon=false \"-Dorg.gradle.jvmargs=-Xmx2G -XX:MaxMetaspaceSize=768m -XX:ReservedCodeCacheSize=256m -XX:ActiveProcessorCount=2\""
cd "$APP_DIR"
"$FLUTTER_ROOT/bin/flutter" build apk --release --target-platform android-arm64 \
  --local-engine "$ENGINE_OUT" --local-engine-host "$HOST_OUT" \
  --local-engine-src-path "$ENGINE_SRC"

OUTPUT="$APP_DIR/build/custom-engine/$BACKEND"
mkdir -p "$OUTPUT"
EXTRA=(--backend "$BACKEND")
if [[ "$BACKEND" == B ]]; then
  # Find the kernel by matching the exact unstripped AOT object copied by the
  # Flutter Android build target, never by guessing the most recently modified dill.
  KERNEL="$(python3 - "$APP_DIR" <<'PY'
import hashlib
from pathlib import Path
import sys
root = Path(sys.argv[1])
staged = root / 'build/app/intermediates/flutter/release/arm64-v8a/app.so'
digest = hashlib.sha256(staged.read_bytes()).digest()
matches = [path.parent.parent / 'app.dill'
           for path in (root / '.dart_tool/flutter_build').glob('*/arm64-v8a/app.so')
           if hashlib.sha256(path.read_bytes()).digest() == digest]
if not matches or len({hashlib.sha256(p.read_bytes()).digest() for p in matches}) != 1:
    raise SystemExit('Cannot uniquely identify the kernel for the APK AOT library')
print(matches[0])
PY
)"
  "$ENGINE_SRC/out/$ENGINE_OUT/clang_x64/gen_snapshot" --deterministic \
    --snapshot_kind=app-aot-assembly --assembly="$OUTPUT/app.S" --strip "$KERNEL"
  EXTRA+=(--assembly "$OUTPUT/app.S")
fi

python3 "$HERE/package_apk.py" \
  --input "$APP_DIR/build/app/outputs/flutter-apk/app-release.apk" \
  --output "$OUTPUT/app-unsigned.apk" "${EXTRA[@]}" \
  --engine-src "$ENGINE_SRC" --dart "$DART" \
  --private "$ASSET_PRIVATE_KEY" --public "$ASSET_PUBLIC_KEY"
"$ANDROID_BUILD_TOOLS/zipalign" -P 16 -f 4 "$OUTPUT/app-unsigned.apk" "$OUTPUT/app-aligned.apk"
"$ANDROID_BUILD_TOOLS/apksigner" sign --ks "$APK_KEYSTORE" \
  --ks-key-alias "$APK_KEY_ALIAS" --ks-pass env:APK_KS_PASSWORD \
  --key-pass env:APK_KEY_PASSWORD --out "$OUTPUT/app-candidate.apk" "$OUTPUT/app-aligned.apk"
"$ANDROID_BUILD_TOOLS/apksigner" verify --verbose "$OUTPUT/app-candidate.apk"
"$ANDROID_BUILD_TOOLS/zipalign" -c -P 16 4 "$OUTPUT/app-candidate.apk"
bash "$HERE/verify_release_apk.sh" "$OUTPUT/app-candidate.apk" --public "$ASSET_PUBLIC_KEY"
echo "Release gate passed: $OUTPUT/app-candidate.apk (device test is separate)"
