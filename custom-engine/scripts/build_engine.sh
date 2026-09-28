#!/usr/bin/env bash
set -euo pipefail

: "${ENGINE_SRC:?Set ENGINE_SRC to the synced engine/src directory}"
: "${ASSET_PUBLIC_KEY:?Set ASSET_PUBLIC_KEY to the release Ed25519 public key}"
ENGINE_OUT="${ENGINE_OUT:-android_release_arm64}"
HOST_OUT="${HOST_OUT:-host_debug_unopt}"
NINJA="${NINJA:-ninja}"

# GN configuration and dependency sync are deliberate prerequisites. Do not
# silently regenerate an existing checkout with different engine feature flags.
if ! grep -Eq '^flutter_custom_asset_hardened = true$' "$ENGINE_SRC/out/$ENGINE_OUT/args.gn"; then
  echo 'Configure the Android release engine with flutter_custom_asset_hardened=true first.' >&2
  exit 2
fi
HEADER="$ENGINE_SRC/flutter/assets/packed_asset_public_key_generated.h"
GENERATED="$(mktemp "${HEADER}.XXXXXX")"
trap 'rm -f "$GENERATED"' EXIT
"$ENGINE_SRC/out/$HOST_OUT/asset_signer" export-header \
  --public "$ASSET_PUBLIC_KEY" \
  --output "$GENERATED"
if ! cmp -s "$GENERATED" "$HEADER"; then
  mv "$GENERATED" "$HEADER"
fi

# flutter.jar also refreshes arm64_v8a_release.jar used by the local Maven repo.
# Building only libflutter.so leaves the APK dependency pointing at old code.
cd "$ENGINE_SRC"
"$NINJA" -C "out/$ENGINE_OUT" -j1 flutter.jar
