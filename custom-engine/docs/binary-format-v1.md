# FEAP payload format v1

Little-endian, fixed-width integers, no C-struct casting. All multi-byte fields
are decoded with explicit byte shifts on both the Dart packer and the C++
resolver, so the format does not depend on struct packing or host endianness.

## Layout

```
+------------------+  offset 0
|   Header (32 B)  |
+------------------+  offset = index_offset (= 32)
|  Index[count]    |  count * 32 B, sorted ascending by key_hash (unsigned)
+------------------+  offset = blob_offset (>= index_end, alignment-padded)
|   Blob           |  per-asset blocks, each starting at its own aligned offset
+------------------+  offset = payload_size
```

## Header (32 bytes)

| Offset | Size | Field         | Notes                                   |
|-------:|-----:|---------------|-----------------------------------------|
| 0      | 4    | magic         | `46 45 41 50` ("FEAP")                  |
| 4      | 2    | version       | `1`                                     |
| 6      | 2    | flags         | reserved, `0` in v1                     |
| 8      | 4    | count         | number of index entries                 |
| 12     | 4    | index_offset  | must equal `32`                         |
| 16     | 8    | blob_offset   | `>= 32 + count*32`, alignment-padded    |
| 24     | 8    | reserved      | `0`                                     |

## Index entry (32 bytes, `count` of them)

| Offset | Size | Field         | Notes                                       |
|-------:|-----:|---------------|---------------------------------------------|
| 0      | 8    | key_hash      | FNV-1a 64 of the asset key                  |
| 8      | 8    | offset        | block start relative to `blob_offset`       |
| 16     | 4    | stored_size   | bytes stored in the blob                    |
| 20     | 4    | original_size | uncompressed size                           |
| 24     | 4    | flags         | codec id in low 8 bits (`kCodecMask=0xff`)  |
| 28     | 4    | reserved      | `0`                                         |

Codec ids: `0` = none (stored raw, zero-copy), `1` = zlib. `2` (lz4) and `3`
(zstd) are reserved. The header `version` is `2` once any codec beyond none may
appear; the resolver accepts versions `1` and `2` (v1 payloads always used codec
`0`). When `stored_size < original_size` and codec is `zlib`, the resolver
inflates into an owned buffer and checks the inflated length equals
`original_size`; a decompressed size above `kMaxDecompressedSize` (256 MiB) or a
zlib error yields `nullptr`, never a crash.

Entries are sorted ascending by `key_hash` treated as unsigned, enabling an
O(log n) binary search at lookup time.

## Asset key

The key is the file path relative to the `flutter_assets` root, using `/` as the
separator (e.g. `assets/images/logo.webp`, `AssetManifest.bin`). This is exactly
the string the framework passes to `AssetResolver::GetAsMapping`. Plaintext keys
are never stored in the payload; only the 64-bit hash is.

## Hashing and collisions

v1 uses FNV-1a 64. The packer treats any FNV-1a collision across two distinct
keys as a fatal build error and aborts, so a shipped payload is collision-free
by construction. A future format revision may switch to xxh3 and add a secondary
fingerprint (length + secondary hash) rather than aborting.

## Alignment

`--alignment N` (power of two, default 16) pads both `blob_offset` and every
block start to an `N`-byte boundary. Because the resolver reads each block's
offset from the index, alignment padding is transparent to it and prepares the
payload for page-aligned mmap in a later milestone.

## Sidecar metadata (`payload_meta.json`)

Emitted next to `payload.bin`, not part of the binary and not read by the engine.
Holds `payloadCrc32`, per-entry `crc32`, sizes, alignment, hash and compression
selectors. Used by CI/integrity tooling and `--inspect`. Keeping integrity data
out of the binary preserves byte-for-byte determinism of `payload.bin` and keeps
the resolver format stable.

## Bounds and validation (resolver side)

The resolver rejects a payload unless: `size >= 32`; magic and version match;
`index_offset == 32`; `index_offset + count*32 <= size` (computed in 64-bit,
overflow-safe); and `index_end <= blob_offset <= size`. At lookup, block bounds
are re-validated with overflow-safe arithmetic (`offset <= size - blob_offset`,
`stored_size <= size - block_start`) before any pointer is formed. A corrupt or
truncated payload yields a controlled `nullptr`/`IsValid() == false`, never an
out-of-range access.

## Compression selection (packer)

`--compression auto|none|zlib`. `auto` skips already-compressed extensions
(png/jpg/jpeg/gif/webp/mp3/aac/mp4/woff2/…) and otherwise stores zlib only when
it saves at least 64 bytes AND at least 5% of the original — a floor derived from
`benchmarks/compression_bench.dart`, where tiny or incompressible inputs
(config.json 38→46, random 8 KiB→+11) actually inflate under zlib while
repetitive JSON/prose/SVG shrink to ~1–9%. zlib is the only codec available in
both the Dart packer (built in) and the engine (in-tree `//flutter/third_party/
zlib`) without new dependencies; zstd/lz4 would need a Dart FFI plus a
third_party GN target and remain reserved codec ids.

## Integrity (implemented)

Per-entry CRC32 of the stored bytes is carried in the entry's last 4 bytes
(offset 28) and gated by header flag bit0 (`kHeaderFlagEntryCrc32`). The resolver
validates it on load and via `VerifyIntegrity()`. This is an accidental-corruption
check, not tamper resistance — see `docs/integrity.md`.

## Not yet honored (planned)

- xxh3 hashing with secondary fingerprint for collision handling.
- Optional authenticated encryption (AES-256-GCM / ChaCha20-Poly1305), whose
  AEAD tag — distinct from the CRC32 — provides authenticity.
