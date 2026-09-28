# Patch organization

The work is structured so each step is independently buildable and testable, in
the safe order correctness → compatibility → measurement → optimization →
hardening. Suggested commit split when upstreaming:

1. **Asset resolver interface + parser** — `asset_resolver.h` enum
   `kPackedAssetProvider`; `packed_asset_resolver.{h,cc}` (header/index parse,
   explicit byte decoding, overflow-safe bounds); host unit tests.
2. **Hashing module** — `asset_hash.{h,cc}` (`asset_hash_arm64` FNV-1a) + tests;
   resolver routes `HashAssetKey` through it.
3. **Packer** — `custom-engine/asset_packer/` (`flutter_asset_packer.dart`,
   `--inspect`, deterministic, alignment, crc32 sidecar) + Dart tests.
4. **Engine integration + Backend A** — `android_shell_holder` wiring (explicit
   `fml::NativeLibrary` load and `__flutter_payload_start/_end` lookup), `linker/payload_section.S`,
   `build_libpayload.sh`; `libflutter.so` relink.
5. **Android packaging / Backend B** — `backend_b_experiment.sh` + docs.
6. **Compression** — codec id in `flags`, zlib inflate, `assets` deps zlib;
   packer `--compression auto|none|zlib`; tests + benchmark.
7. **Cache** — `packed_asset_cache.{h,cc}` + tests; resolver cache path; wiring.
8. **Integrity** — per-entry CRC32, header flag, `VerifyIntegrity()`, fuzz test.
9. **Encryption** — `packed_asset_crypto.{h,cc}` (BoringSSL), `asset_encryptor`,
   resolver decrypt; tests + integration. The demonstration XOR key was removed
   when the offline signing path landed.
10. **Benchmark / verification** — `compression_bench.dart`, host suite script,
    `verify_release_apk.sh`.
11. **ARM64** — evaluated; kept C++ (see docs/arm64-optimization.md).
12. **Offline authentication** — signed v3 metadata and block digests,
    `asset_signer` host tool, hardened Android public-key wiring, release gate;
    remove the Android demo XOR key.

Each of steps 1–9 was built for the host, unit-tested, and (for the engine-side
changes) relinked into `libflutter.so` for arm64 before moving on. The single
tracked-file patch is in `engine_patch/`; the new engine sources are mirrored in
`runtime/` and installed by `engine_patch/apply.sh`.

## What remains (device-gated)

Packaging a release APK with `libpayload.so`, launching it on an Android arm64
device/emulator to confirm `Image.asset` / `rootBundle.load` resolve from the
payload with `assets/flutter_assets/` absent, and running
`scripts/verify_release_apk.sh` against the built APK. All resolver, format,
compression, integrity, cache, and encryption behavior is already verified
host-side against the real engine classes.
