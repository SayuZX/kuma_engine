# ARM64 optimization decision

Following the rule "profile first; add assembly only for a real hotspot with a
measurable, testable benefit; if the compiler's ARM64 output is as fast, keep
C++."

## Candidate hot paths in the load pipeline

| Stage | Implementation | SIMD already? |
|---|---|---|
| key hash (FNV-1a 64) | `asset_hash_arm64` (scalar C++) | serial by design |
| integrity (CRC32) | zlib `crc32` | yes (zlib SIMD) |
| decompress | zlib `uncompress` | yes (zlib SIMD) |
| decrypt/authenticate | BoringSSL ChaCha20-Poly1305 | yes (BoringSSL asm) |
| copy | none for uncompressed (zero-copy) | n/a |

CRC32, decompression and decryption — the CPU-heavy stages — already run
vendor-optimized SIMD/assembly inside zlib and BoringSSL. There is nothing to
hand-write there. The only first-party arithmetic is the key hash.

## Why FNV-1a is not an assembly candidate

FNV-1a is a strict serial dependency chain (`h = (h ^ b) * prime` per byte), so
SIMD/NEON cannot parallelize a single hash. Asset keys are short (tens of
bytes), so hashing is a negligible fraction of a load dominated by inflate and
decrypt. A hand-written ARM64 loop would only reproduce what the compiler
already emits.

`AssetHashTest.ThroughputDiagnostic` measures ~0.3 GB/s under x86_64 Rosetta on
this host (native arm64 is faster); for a ~30-byte key that is far below the
cost of a single inflate or AEAD open. Hashing does not appear as a hotspot.

## What was built instead

- `asset_hash_arm64` (extern "C", `assets/asset_hash.cc`) — the stable ABI the
  spec asked for, currently a portable scalar C++ implementation used by the
  resolver so there is a single source of truth for the key hash.
- `AssetHashTest.MatchesKnownFnv1a64Vectors` locks it to the published FNV-1a 64
  vectors ("" , "a", "foobar").
- `AssetHashTest.ArmApiMatchesReferenceOnRandomInputs` differentially checks the
  shipping function against an independent reference over 100k random inputs —
  the harness that would validate any future assembly variant.
- Dart↔C++ hash equivalence is already proven continuously: every integration
  run looks assets up by the packer's hash, so a divergence would fail lookups.

## Decision

Keep the scalar C++ hash. No hand-written ARM64 assembly is added, because the
profile shows no first-party hotspot and FNV-1a offers no SIMD headroom. The
extern "C" seam and differential harness are in place, so an assembly variant
can be dropped in and validated later if a real device profile (simpleperf /
Perfetto) ever shows the hash mattering.
