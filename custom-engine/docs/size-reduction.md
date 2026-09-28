# Engine size analysis

Measured on the pinned `android_release_arm64` build (`args.gn`:
`is_official_build=true`, `flutter_runtime_mode=release`,
`optimize_for_size=false`, `stripped_symbols=false` with a separate stripped
output shipped in `lib.stripped/`).

## Measured, applied (safe first)

| Lever | Before | After | Saving | Risk |
|---|---:|---:|---:|---|
| Symbol strip (current hardened build, unstripped → shipped `lib.stripped/libflutter.so`) | 327,192,888 | 13,259,584 | 313,933,304 (95.9%) | none — release ships the stripped lib |
| Exported-symbol minimization (`android_exports.lst` version script) | — | 70 defined dynamic symbols | no asset-specific exports | keeps JNI/embedding exports intact |
| Section GC + hidden visibility (`--gc-sections`, `-fvisibility=hidden`) | — | — | dead sections dropped | none — already in release config |

The dominant, safe win is stripping; it is already in effect for the shipped
library. Every symbol our code adds is internal: `nm -D` shows zero
`PackedAsset*` / `asset_hash*` / signature symbols exported. This build has 70
defined dynamic symbols; the earlier 68 count was not reproduced with the same
counting command and should not be used as an ABI delta.

## Footprint of the packed-asset system itself

| Library | Stripped size | Δ vs upstream baseline |
|---|---:|---:|
| baseline (upstream) | 13,249,920 | — |
| + resolver + cache + CRC32 + AEAD wrapper + hash | 13,259,136 | +9,216 bytes |
| + signed v3 + hardened Android wiring | 13,259,456 | +9,536 bytes |
| + explicit Backend A/B library loading | 13,259,584 | +9,664 bytes |

The Ed25519/SHA-256 authenticity path added 320 bytes over the previous
stripped prototype under the pinned non-LTO build configuration. Explicit
native library loading adds a further 128 bytes; the whole asset system adds
9,664 bytes against the earlier upstream measurement.
zlib and BoringSSL were already linked into `libflutter.so`; the index's
`16×asset_count + 64` bytes are payload data, not engine code.

## Available but NOT applied (each needs build + test + measure; higher risk)

These are levers for shrinking the *engine*, orthogonal to the asset system.
They are listed, not executed, because each removes engine capability and must be
validated against the full test suite and a running app before it can ship:

| Lever | Expected effect | Risk |
|---|---|---|
| `optimize_for_size = true` (`-Oz`) | smaller `.text` | medium — runtime perf regression |
| Drop unused renderer backend (e.g. Skia when Impeller-only) | large `.text`/`.rodata` cut | high — breaks fallback rendering |
| Trim ICU data (`icudtl.dat`, 9.3 MB, ships separately) | large asset cut | high — breaks locale/text APIs |
| Remove unused image codecs | moderate | medium — breaks those `Image` formats |
| Strip tracing/diagnostics in release | small | low–medium — loses timeline/profiling |

Recommendation: the asset system's own footprint is already negligible and its
ABI is minimal. Engine-wide size cuts should be pursued only with per-lever
build/test/size comparison, starting with the low-risk ones, and are out of
scope for the asset pipeline.
