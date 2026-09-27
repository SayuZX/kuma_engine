# Decompressed-asset cache

`PackedAssetCache` is an optional, thread-safe LRU cache of decompressed asset
buffers. It exists to avoid re-inflating the same compressed asset on repeated
loads. It is off unless a resolver is constructed with a cache.

## Scope: compressed assets only

Uncompressed (codec none) assets are already returned as zero-copy
`NonOwnedMapping`s pointing straight into the payload, so there is nothing to
cache and the resolver never touches the cache for them (verified by
`CacheNotUsedForUncompressedAssets`). Only zlib assets go through the cache.

## Lifetime (no use-after-free)

Entries are `shared_ptr<const std::vector<uint8_t>>`. On a hit the resolver
returns a `NonOwnedMapping` whose release proc captures the `shared_ptr`, so the
buffer lives exactly as long as any mapping that references it — even if the LRU
evicts the entry in the meantime. `EvictedBufferStaysAliveForHolder` pins this
guarantee. A cache hit returns the same pointer as the buffer just inserted
(`CacheServesCompressedAssetOnSecondRead`), so hits are zero-copy.

## Eviction and bounds

- `capacity_bytes`: total decompressed bytes held; LRU eviction from the back
  until an incoming item fits.
- `max_item_bytes`: items larger than this are not cached at all (still returned
  to the caller for that load), so a few large assets cannot evict everything —
  "don't cache large assets aggressively". Counted as `skipped_too_large`.
- An item larger than the whole capacity is likewise skipped.

The Android wiring uses 16 MiB capacity and a 4 MiB per-item cap.

## Threading (see also docs/architecture.md)

A single `std::mutex` guards the list, index and counters. Inflation happens
outside the lock; only the map/list mutation is locked, keeping the hot path
short. `ConcurrentAccessDoesNotCrash` exercises 8 threads doing mixed get/put.
Two threads missing the same key may both inflate once; the last `Put` wins and
both callers get correct data.

## Statistics

`GetStats()` returns hits, misses, evictions, insertions, skipped_too_large,
bytes_used and capacity_bytes for tuning and diagnostics.
