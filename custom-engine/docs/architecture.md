# Custom packed-asset engine — architecture

Target: Android arm64, Flutter release AOT, custom Flutter Engine. Goal: ship
Flutter apps whose assets live in a native packed payload instead of
`assets/flutter_assets/`, while `Image.asset` / `rootBundle.load` keep working
with no application-layer API change.

Prototype verification used Flutter `flutter-3.47-candidate.0`, engine build content_hash
`ab598368...`, Dart `b530c21f...`.

## Component map

```
Dart / framework      rootBundle.load / Image.asset            (unchanged)
  |
engine boundary       ui.ImmutableBuffer.fromAsset (dart:ui)   (unchanged)
  |
AssetManager          iterates resolvers, first non-null wins  assets/asset_manager.cc:63
  |
PackedAssetResolver   parses index, binary-search, bounds      assets/packed_asset_resolver.{h,cc}
  |
payload bytes         const uint8_t* + size (platform-supplied)
  |
Backend A/B           .flutter_payload section in a native .so
```

The resolver is placed in `//flutter/assets/` (platform-neutral, host-testable),
not under `shell/platform/android/`. Only payload discovery and registration are
Android-specific and live in `android_shell_holder.cc::BuildRunConfiguration`,
where the packed resolver is pushed **before** the APK resolver, giving order
`[packed, apk]`: packed is tried first, APK is the fallback. `AddAssetResolver`
is `PushBack` and rejects `!IsValid()`, so if no payload is linked the resolver
is invalid and simply not added — behavior is then identical to upstream.

## Runtime pipeline

1. Startup: construct resolver from `(payload, size)`; validate header + index
   bounds and, for signed v3, verify the compact index/digest-table signature.
   No blob scan, decompress, or decrypt at startup.
2. Lookup (`GetAsMapping(key)`): `hash = FNV-1a64(key)`, binary search the index,
   re-validate block bounds, return a mapping.
3. A signed v3 block is checked against its signed SHA-256/128 digest. Plain
   uncompressed blocks return a zero-copy `fml::NonOwnedMapping`; compressed or
   encrypted blocks return a mapping that owns the decoded buffer.

## Ownership / lifetime

- The payload is a read-only region owned by the dynamic linker (mmap of the
  `.so` section); its lifetime is the process. The resolver borrows a
  `const uint8_t*` + size and never owns or frees it.
- The resolver is owned by `AssetManager` via `unique_ptr`; it lives as long as
  the isolate's asset manager.
- Zero-copy mappings point into the payload region and need no release proc,
  because that region outlives every mapping. Decoded buffers use a shared
  owner retained by the mapping and optional LRU cache.

## Threading

- Construction happens once on the platform thread inside `BuildRunConfiguration`.
- After construction the resolver is immutable (`payload_`, `size_`, `count_`,
  offsets are const/settled), so concurrent `GetAsMapping` calls from UI/IO
  threads are lock-free and safe.
- The optional decompressed-asset cache owns its synchronization and stays off
  the zero-copy hot path.

## Error handling

All header and per-block offsets are validated with overflow-safe 64-bit
arithmetic before a pointer is formed. Corrupt, truncated, or malformed payloads
produce `IsValid() == false` or a `nullptr` mapping, never an out-of-range read.
Diagnostics use `FML_DLOG` (debug-only) and never print asset names or paths, so
release builds emit nothing and leak no key strings.

## Build / test pipeline (current)

- Packer: `custom-engine/asset_packer/flutter_asset_packer.dart`
  (`--input/--output/--inspect/--hash/--compression/--alignment`), deterministic,
  emits `payload.bin` + `payload_meta.json`.
- Engine integration: `custom-engine/engine_patch/` (patch for tracked files +
  `runtime/` mirror of the new sources + `apply.sh`).
- Host verification (no device, no Metal toolchain needed):
  `custom-engine/scripts/build_host_test.sh` runs the C++ unit tests, the Dart
  packer tests, and the packer→resolver integration test.
- Release APK gate: `custom-engine/scripts/verify_release_apk.sh` (AA).

## Known limitations / non-goals

- Signed v3 authenticates metadata and stored blocks offline. It does not
  conceal bytes or make a client-side decryption key secret.
- v1/v2 remain supported for development. Hardened Android accepts signed v3
  only; see `signed-format-v3.md`. The current v3 path authenticates but does
  not conceal raw asset bytes.
- End-to-end proof on a device (real APK, `Image.asset` on screen) is a later
  milestone; current proof is at the engine-resolver and packer level.
- Standard `assets_unittests` target pulls Impeller (Metal) on this host; the
  lean `packed_asset_resolver_smoketest` target exists to run without a Metal
  toolchain.
