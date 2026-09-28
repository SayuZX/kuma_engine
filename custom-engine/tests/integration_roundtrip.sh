#!/usr/bin/env bash
set -euo pipefail

ENGINE_ROOT="${ENGINE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
ENGINE_SRC="${ENGINE_SRC:-$(dirname "$ENGINE_ROOT")}"
HOST_OUT="${HOST_OUT:-out/host_debug_unopt}"
DART="${DART:-dart}"
PACKER="$ENGINE_ROOT/custom-engine/asset_packer/flutter_asset_packer.dart"

export PATH="$HOME/development/depot_tools:$PATH"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> building fixture assets"
mkdir -p "$WORK/in/assets/data" "$WORK/in/assets/images"
printf 'Hello from packed payload!\n' > "$WORK/in/assets/hello.txt"
printf '{"debug":false,"level":3}' > "$WORK/in/assets/data/config.json"
head -c 4096 /dev/urandom > "$WORK/in/assets/images/blob.bin"
printf 'manifest-bytes-xyz' > "$WORK/in/AssetManifest.bin"
python3 -c "open('$WORK/in/assets/big.json','w').write('{\"row\":123456}\n'*800)"

echo "==> packing with flutter_asset_packer (auto compression)"
"$DART" "$PACKER" --input "$WORK/in" --output "$WORK/out" \
  --compression auto --alignment 16

echo "==> building filecheck, asset_encryptor and asset_signer"
if [[ ! -f "$ENGINE_SRC/flutter/tools/gn" ]]; then
  echo "ENGINE_SRC must point to a synced engine workspace containing flutter/tools/gn" >&2
  exit 2
fi
cd "$ENGINE_SRC"
ninja -C "$HOST_OUT" packed_asset_resolver_filecheck asset_encryptor \
  asset_signer >/dev/null

echo "==> running real engine resolver against packer output"
"$DART" "$PACKER" --inspect "$WORK/out/payload.bin" --against "$WORK/in"

pairs=(
  "assets/hello.txt" "$WORK/in/assets/hello.txt"
  "assets/data/config.json" "$WORK/in/assets/data/config.json"
  "assets/images/blob.bin" "$WORK/in/assets/images/blob.bin"
  "assets/big.json" "$WORK/in/assets/big.json"
  "AssetManifest.bin" "$WORK/in/AssetManifest.bin"
)

"./$HOST_OUT/packed_asset_resolver_filecheck" "$WORK/out/payload.bin" "${pairs[@]}"

echo "==> encrypting payload (ChaCha20-Poly1305 via BoringSSL) and verifying"
KEY="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
"./$HOST_OUT/asset_encryptor" --input "$WORK/out/payload.bin" \
  --output "$WORK/out/payload.enc.bin" --key "$KEY" --alignment 16

echo "==> resolver WITHOUT key must fail on encrypted payload"
if "./$HOST_OUT/packed_asset_resolver_filecheck" "$WORK/out/payload.enc.bin" \
    "assets/hello.txt" "$WORK/in/assets/hello.txt" >/dev/null 2>&1; then
  echo "FAIL encrypted payload readable without key"
  exit 1
fi
echo "ok   encrypted payload not readable without key"

echo "==> resolver WITH key reads encrypted payload"
"./$HOST_OUT/packed_asset_resolver_filecheck" --key "$KEY" \
  "$WORK/out/payload.enc.bin" "${pairs[@]}"

echo "==> signing index and block digests with an ephemeral Ed25519 key"
"./$HOST_OUT/asset_signer" keygen \
  --private "$WORK/signing.ed25519-private" \
  --public "$WORK/signing.ed25519-public"
python3 - "$WORK/out/payload.bin" "$WORK/out/payload.corrupt.bin" <<'PY'
import sys
source = bytearray(open(sys.argv[1], 'rb').read())
source[-1] ^= 1
open(sys.argv[2], 'wb').write(source)
PY
if "./$HOST_OUT/asset_signer" sign --input "$WORK/out/payload.corrupt.bin" \
    --output "$WORK/out/should-not-sign.bin" \
    --private "$WORK/signing.ed25519-private" >/dev/null 2>&1; then
  echo "FAIL signer accepted a corrupt source payload"
  exit 1
fi
"./$HOST_OUT/asset_signer" sign \
  --input "$WORK/out/payload.bin" --output "$WORK/out/payload.signed.bin" \
  --private "$WORK/signing.ed25519-private"
"./$HOST_OUT/asset_signer" sign \
  --input "$WORK/out/payload.bin" --output "$WORK/out/payload.signed-again.bin" \
  --private "$WORK/signing.ed25519-private"
cmp "$WORK/out/payload.signed.bin" "$WORK/out/payload.signed-again.bin"
"./$HOST_OUT/asset_signer" verify --input "$WORK/out/payload.signed.bin" \
  --public "$WORK/signing.ed25519-public"
"./$HOST_OUT/asset_signer" verify --index-only \
  --input "$WORK/out/payload.signed.bin" \
  --public "$WORK/signing.ed25519-public"
"./$HOST_OUT/asset_signer" export-header \
  --public "$WORK/signing.ed25519-public" \
  --output "$WORK/packed_asset_public_key_generated.h"
"./$HOST_OUT/packed_asset_resolver_filecheck" --public "$WORK/signing.ed25519-public" \
  "$WORK/out/payload.signed.bin" "${pairs[@]}"

echo "==> signed payload rejects index and block tampering"
python3 - "$WORK/out/payload.signed.bin" "$WORK/out/index-tampered.bin" \
  "$WORK/out/block-tampered.bin" <<'PY'
import sys
source = bytearray(open(sys.argv[1], 'rb').read())
index = bytearray(source)
index[32] ^= 1
open(sys.argv[2], 'wb').write(index)
source[-1] ^= 1
open(sys.argv[3], 'wb').write(source)
PY
for bad in "$WORK/out/index-tampered.bin" "$WORK/out/block-tampered.bin"; do
  if "./$HOST_OUT/packed_asset_resolver_filecheck" \
      --public "$WORK/signing.ed25519-public" "$bad" \
      "assets/big.json" "$WORK/in/assets/big.json" >/dev/null 2>&1; then
    echo "FAIL signed payload accepted tampering: $bad"
    exit 1
  fi
  if "./$HOST_OUT/asset_signer" verify --input "$bad" \
      --public "$WORK/signing.ed25519-public" >/dev/null 2>&1; then
    echo "FAIL signer accepted tampering: $bad"
    exit 1
  fi
done
if "./$HOST_OUT/asset_signer" verify --index-only \
    --input "$WORK/out/index-tampered.bin" \
    --public "$WORK/signing.ed25519-public" >/dev/null 2>&1; then
  echo "FAIL index-only verification accepted metadata tampering"
  exit 1
fi
"./$HOST_OUT/asset_signer" verify --index-only \
  --input "$WORK/out/block-tampered.bin" \
  --public "$WORK/signing.ed25519-public" >/dev/null
echo "ok   block verification remains lazy at resolver initialization"
echo "ok   metadata and block tampering rejected"

echo "==> signed encrypted payload authenticates before decrypting"
"./$HOST_OUT/asset_signer" sign \
  --input "$WORK/out/payload.enc.bin" \
  --output "$WORK/out/payload.enc.signed.bin" \
  --private "$WORK/signing.ed25519-private"
"./$HOST_OUT/packed_asset_resolver_filecheck" \
  --public "$WORK/signing.ed25519-public" --key "$KEY" \
  "$WORK/out/payload.enc.signed.bin" "${pairs[@]}"
