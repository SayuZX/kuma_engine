# Memory mapping and zero-copy

## What the current design already gets from the loader

The payload lives in the `.flutter_payload` section, which is `PROGBITS` with the
`ALLOC` flag inside a read-only `PT_LOAD` segment (verified by
`build_libpayload.sh` and `backend_b_experiment.sh`). When Android's dynamic
linker loads the `.so`, that segment is **mmap'd read-only**; the OS demand-pages
it. So:

- The payload is memory-mapped by the loader — no separate `mmap()` call, no file
  descriptor to manage, no copy of the whole payload onto the heap.
- The resolver reads only the header, the index, and the requested block, so only
  those pages are faulted in ("read only requested block").
- Uncompressed assets return a `NonOwnedMapping` pointing straight into the mapped
  section — genuinely zero-copy (section N).
- Bounds are validated with overflow-safe arithmetic before any pointer into the
  mapping is formed; a corrupt payload can never read outside the mapped range.
- Blocks are 16-byte aligned and the section is page-aligned by the loader.

## Copies that still happen (documented, section N)

| Path | Copy? | Why |
|---|---|---|
| uncompressed, unencrypted | none (zero-copy) | mapping points into the section |
| compressed | one owned buffer | inflate must allocate its output |
| encrypted | one owned buffer | AEAD open must allocate plaintext |
| cache hit | none (zero-copy) | mapping points into the cached buffer |

The decode buffers are owned via `shared_ptr` and kept alive for the mapping's
lifetime; the LRU cache makes repeat reads of compressed/encrypted assets
zero-copy after the first.

## Explicit mmap — when it would be needed

An explicit `mmap()` (open the `.so`/APK, `mmap` the payload byte range, optional
`madvise`) is only required when the payload is **not** inside a loaded section —
for example if it were stored as a plain APK entry rather than a section in a
native library. That path would need to own:

- the file descriptor lifecycle (open once, close when done),
- page-aligned offset/length for the `mmap` call,
- the mapping lifetime (unmap only after all `NonOwnedMapping`s are gone),
- thread-safe one-time setup (`std::call_once`).

Because both Backend A (`libpayload.so`) and Backend B (payload in `libapp.so`)
keep the payload in a loaded section, the loader's mapping already satisfies the
mmap goals and no explicit `mmap`/fd management is introduced. If a future
backend stores the payload outside a loaded section, add the explicit path with
the ownership rules above.
