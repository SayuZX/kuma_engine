# Offline signed payload (format v3)

The hardened Android resolver trusts one Ed25519 public key compiled into
`libflutter.so`. The private signing key stays outside the repository and is
needed only when producing a payload. This adds offline authenticity to the
index and stored asset blocks. It does not hide the asset bytes or prevent an
attacker who can replace the engine binary from replacing its trust anchor.

## Binary layout

All integers are little-endian, decoded byte by byte. No C++ struct is cast
over the input. Offsets are from the start of the payload unless noted.

| Region | Size | Meaning |
| --- | ---: | --- |
| header | 32 B | FEAP magic; version=3; flags include bit 2 (signed); count; index offset=32; blob offset; digest-table offset at byte 24 |
| sorted index | count × 32 B | existing v2 entry layout; entry CRC field is zero in signer output |
| digest table | count × 16 B | first 16 bytes of SHA-256 over each stored block, in index order |
| signature | 64 B | Ed25519 signature over every byte from offset 0 through the end of the digest table |
| padding | 0–alignment−1 B | zero filled; blob offset is aligned |
| blob | variable | existing independently addressable stored blocks |

`digest_table_offset` must equal `32 + count×32`; the signature immediately
follows the digest table. The resolver rejects unexpected flags, truncated
regions, a missing public key, an unsigned payload supplied with a public key,
and invalid signatures. Block bounds and each block's digest are checked when
that asset is requested. `asset_signer verify` scans all blocks for the release
gate. Metadata authentication adds `16×count + 64 + padding` bytes relative to
v2. An unchanged source and signing key produce identical bytes.

## Host verification benchmark

Run `python3 custom-engine/benchmarks/signed_index_bench.py --signer
<path/to/asset_signer>` to reproduce the synthetic v2→v3 size and verification
test. Each case uses independently addressable one-byte blocks, seven process
runs, and reports median/min/max wall-clock milliseconds. The measured host is
x86_64 under Rosetta on macOS; process startup and file I/O are included.

| Assets | v2 bytes | v3 bytes | Index only median (min–max) ms | All blocks median (min–max) ms |
| ---: | ---: | ---: | ---: | ---: |
| 100 | 3,332 | 4,996 | 15.982 (15.158–20.167) | 20.980 (18.085–24.573) |
| 1,000 | 33,032 | 49,096 | 25.079 (21.063–26.096) | 20.606 (17.370–29.284) |
| 10,000 | 330,032 | 490,096 | 29.841 (25.980–43.095) | 28.179 (26.423–32.091) |
| 50,000 | 1,650,032 | 2,450,096 | 48.880 (46.197–60.415) | 68.094 (64.104–72.354) |

The 1,000-item inversion reflects startup and scheduling noise; these timings
are an end-to-end host-tool diagnostic, not Android cold-start or in-process
resolver latency. The v3 size increase is exactly `16×count + 64` for these
aligned synthetic inputs. Measure app startup and asset latency on device
before changing the runtime hot path.

## Build and verify

From a synced engine checkout with the host tools built:

```bash
out/host_debug_unopt/asset_signer keygen \
  --private /secure/build/payload.ed25519-private \
  --public /secure/build/payload.ed25519-public
out/host_debug_unopt/asset_signer sign \
  --input build/custom_assets/payload.bin \
  --output build/custom_assets/payload.signed.bin \
  --private /secure/build/payload.ed25519-private
out/host_debug_unopt/asset_signer export-header \
  --public /secure/build/payload.ed25519-public \
  --output flutter/assets/packed_asset_public_key_generated.h
out/host_debug_unopt/asset_signer verify \
  --input build/custom_assets/payload.signed.bin \
  --public /secure/build/payload.ed25519-public
```

`keygen` creates the private file with mode 0600 and refuses to overwrite it.
Do not commit the private key. Keep the generated public header and signing key
paired for each release. Build Android with GN argument
`flutter_custom_asset_hardened=true`; compilation fails without the generated
public header. This mode accepts only v3 signed payloads and omits the APK
asset fallback. Development builds retain the previous packed-then-APK order.

Embed `payload.signed.bin` with `linker/build_libpayload.sh`, then package the
resulting `libpayload.so` alongside the engine. After a release APK exists, run:

```bash
LLVM_READELF=/path/to/llvm-readelf \
LLVM_OBJCOPY=/path/to/llvm-objcopy \
ASSET_SIGNER=/path/to/asset_signer \
bash custom-engine/scripts/verify_release_apk.sh app-release.apk \
  --public /secure/build/payload.ed25519-public
```

The gate extracts `.flutter_payload` from the APK's native library and checks
the signature plus every stored block. It also rejects packaged
`assets/flutter_assets/`, a second payload section, other ABIs, debug info in
`libflutter.so`, and plaintext asset paths found by its static scan.

## Performance and security limits

Startup verifies only the compact metadata signature; asset digests are lazy.
Uncompressed assets stay in the ELF mapping. Verification reads each requested
stored block once before handing it to Flutter. No custom assembly or language
runtime is added: BoringSSL supplies Ed25519 and SHA-256 in the existing native
dependency. Repeated reads of an uncached block repeat the digest check.

The former demo XOR key in the Android shell holder has been removed.
ChaCha20-Poly1305 remains an optional resolver/build-tool capability, but the
hardened Android wiring deliberately has no embedded decryption key. A fully
offline app cannot keep such a key secret against an attacker who controls the
device and binary. Signed payloads provide integrity and origin checking, not
confidentiality; raw asset bytes may still be extracted from the ELF.
