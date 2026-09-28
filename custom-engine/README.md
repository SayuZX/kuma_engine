# custom-engine — Flutter packed-asset pipeline

Replaces Android `assets/flutter_assets/` with a native packed payload read by a
custom Flutter Engine `AssetResolver`, keeping `Image.asset` / `rootBundle.load`
working unchanged. This branch is based on `kuma_engine`'s
`flutter-3.27-candidate.0` branch (`82bd5b7`). The prototype was built and
tested against a separate Flutter 3.47 engine checkout; the Android engine build
and APK runtime have **not** been verified against this 3.27 port. See `docs/`
for the format and architecture. This raises the cost of static extraction; it
does not establish a security boundary.

## Layout

```
custom-engine/
├── asset_packer/     flutter_asset_packer CLI (Dart) + tests
│   ├── packer_format.dart
│   ├── flutter_asset_packer.dart
│   └── packer_test.dart
├── runtime/          canonical mirror of the engine-side resolver sources
├── engine_patch/     patch for tracked engine files + apply.sh
├── linker/           ELF section source and Backend A/B experiments
├── tests/            integration_roundtrip.sh (packer -> real resolver)
├── scripts/          build_host_test.sh, verify_release_apk.sh
├── benchmarks/       compression measurements
└── docs/             architecture.md, binary-format-v1.md
```

The authoritative engine sources live in `assets/`
(`packed_asset_resolver.{h,cc}`, `_unittests.cc`, `_filecheck.cc`); `runtime/`
holds identical copies so `engine_patch/apply.sh` can install them into a fresh
checkout at the pinned 3.27 base. Run `build_host_test.sh` only from a synced
engine workspace with this repo checked out as `flutter/`; set `ENGINE_SRC` to
that workspace and `DART` to a Dart SDK executable.

## Status

The checked results and size numbers below were measured on the separate
Flutter 3.47 engine checkout. This 3.27 branch has passed the Dart packer tests
and Git patch checks; a native 3.27 build, APK inspection, and device run remain
unverified because this repository clone has no synced engine dependencies.

- [x] Milestone 1: real engine `PackedAssetResolver` returns a packed asset;
      8 host unit tests pass; resolver + Android wiring cross-compile for arm64.
- [x] Packer CLI (`--inspect`, deterministic, alignment, crc32 sidecar).
- [x] Native integration test: `flutter_asset_packer` output consumed by the
      real engine resolver, byte-identical.
- [x] Backend A (payload side): `libpayload.so` (arm64) with `.flutter_payload`
      section + exported `__flutter_payload_start/_end`, statically verified;
      host test reads assets from the section via the real resolver.
- [x] `libflutter.so` relinked (arm64) with the resolver integrated: clean build,
      +2104 bytes stripped, zero new exported symbols (ABI unchanged).
- [x] Backend B experiment: link-time embed into `libapp.so` works (payload in a
      PT_LOAD segment); objcopy post-link does not (documented constraint).
- [x] Backend B real AOT relink: a Flutter demo's generated assembly plus signed
      payload links into a 3,974,360 B library; snapshot exports, read-only
      payload segment, 16 KiB alignment and deterministic fixture linking pass.
      Android execution remains unverified.
- [x] APK repack stage: synthetic Backend A/B APKs pass structural/integrity
      tests with the standard asset bundle removed. Build/signing wrappers and
      strict remaining gates are documented in `docs/android-packaging.md`.
- [x] Actual Backend B APK candidate: 16,982,773 B, exactly two ARM64 native
      libraries, no `flutter_assets`, Android v2/v3 signatures and payload
      integrity verified. **The strict plaintext gate fails on Dart asset path
      literals.** This is a test-signed candidate, not a validated release.
- [x] Compression (zlib): packer `--compression auto|none|zlib` with a
      benchmark-derived threshold; resolver inflates (codec id in `flags`,
      format v2, accepts v1); verified end-to-end (12000 B JSON → 66 B stored,
      read back byte-identical). zstd/lz4 reserved (not in-tree). `libflutter.so`
      relinked, +2744 B stripped.
- [x] In-binary integrity: per-entry CRC32 (header-flag gated), validated on
      load + `VerifyIntegrity()`; packer `--inspect` shows per-entry crc ok/BAD;
      fuzz test (5000 malformed payloads) never crashes. CRC32 = corruption
      check, not auth (see docs/integrity.md).
- [x] LRU cache of decompressed assets (`PackedAssetCache`): optional,
      thread-safe, byte-capped, large-item bypass, hit/miss/eviction stats;
      zero-copy hits, no use-after-free on eviction. Wired at 16 MiB / 4 MiB in
      the Android shell holder.
- [x] Encryption (AEAD, optional): ChaCha20-Poly1305 via in-tree BoringSSL on
      both sides — `asset_encryptor` build tool + resolver decrypt. Order
      compress→encrypt→store; read authenticate→decrypt→decompress. The former
      Android demo XOR key has been removed. End-to-end proven: without key the
      encrypted payload is unreadable, with key all assets read byte-identical;
      wrong key / tampered block → authentication failure. See docs/encryption.md.
- [x] Offline signed format v3: Ed25519-authenticated index and per-block
      SHA-256/128 digests; private key kept at build time, public key compiled
      into hardened Android engine. Hardened mode requires a signed payload and
      has no APK asset fallback. Host round-trip and tamper rejection pass.
      See docs/signed-format-v3.md.
- [x] mmap / zero-copy: payload section is loader-mmap'd read-only; uncompressed
      and cache-hit reads are zero-copy; copies documented (docs/mmap-zero-copy.md).
- [x] Size analysis: strip removes 95.9%; whole system adds +9,216 B stripped;
      0 new exported symbols (docs/size-reduction.md).
- [x] ARM64: profiled; FNV-1a is serial and not a hotspot, zlib/BoringSSL already
      SIMD — keep C++. `asset_hash_arm64` ABI + differential/known-vector tests
      in place for a future asm variant (docs/arm64-optimization.md).
- [x] Reproducible build: pinned revisions in `engine_build_config.json`.
- [ ] Runtime end-to-end (device-gated): install the Backend B demo candidate,
      confirm `Image.asset` / `rootBundle.load` and fonts on Android, then measure
      startup, latency and memory. ADB currently reports no connected device.
- [ ] Hardened release: resolve the strict plaintext gate and verify the native
      3.27 port before calling this branch production ready.

## Run the host tests (no device, no Metal toolchain)

```bash
bash custom-engine/scripts/build_host_test.sh
```

## Pack a Flutter asset bundle

```bash
DART=$(command -v dart)
$DART custom-engine/asset_packer/flutter_asset_packer.dart \
  --input build/flutter_assets --output build/custom_assets \
  --hash fnv1a --compression none --alignment 16

$DART custom-engine/asset_packer/flutter_asset_packer.dart \
  --inspect build/custom_assets/payload.bin --against build/flutter_assets
```

## Apply the engine patch to a fresh checkout

```bash
bash custom-engine/engine_patch/apply.sh
```
