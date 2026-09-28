# Backend B — embedding the payload inside libapp.so

Backend B avoids shipping a separate `libpayload.so` by placing the packed
payload inside the Dart AOT library `libapp.so`. The Android holder first opens
`libpayload.so` for Backend A, then tries `libapp.so`; it resolves
`__flutter_payload_start` / `__flutter_payload_end` from the selected handle.
Only the packaging and link step differ.

## Experimental result

`linker/backend_b_experiment.sh` builds a stand-in `libapp.so` (arm64) and embeds
the payload two ways, then checks whether `.flutter_payload` lands in a loadable
`PT_LOAD` segment.

| Method | Section in PT_LOAD? | Runtime-usable symbols? |
|---|---|---|
| `llvm-objcopy --add-section` (post-link) | **No** (segment `None`) | No |
| link `payload_section.o` into the app | **Yes** (PT_LOAD, R) | Yes (`end-start` = size) |

### Why objcopy alone fails

`--add-section` inserts the bytes into the file and can set the `ALLOC` flag, but
it does **not** extend or create a `PT_LOAD` program header to cover them. At
runtime the dynamic linker maps segments, not sections, so the added section is
never mapped; the `--add-symbol` bracket symbols resolve to file offsets that are
not backed by loaded memory. The file passes static `readelf -S` checks yet is
broken at load time. This is the ELF-packaging constraint to be aware of.

### The correct mechanism

Link `payload_section.o` (from `linker/payload_section.S`) into the app library
at link time. The linker then places `.flutter_payload` into a normal read-only
`PT_LOAD` segment and assigns real virtual addresses to the bracket symbols,
which is what the engine wiring needs.

## Integrating with a real app

`gen_snapshot` can emit the AOT output either as a finished ELF `app.so`
(`--snapshot_kind=app-aot-elf`) or as assembly to be linked by the NDK
(`--snapshot_kind=app-aot-assembly`). Backend B needs a link step to add
`payload_section.o`:

- **Assembly path**: hook the Flutter tool so the NDK link of `app.S` also
  includes `payload_section.o` and exports the two symbols. This is the clean
  integration point; it is a build-pipeline change in the Flutter tool, deferred
  to the end-to-end milestone.
- **ELF path**: `gen_snapshot` links `app.so` itself, leaving no external link
  step; post-processing with objcopy is insufficient (above), so this path would
  need a section injected before/without a relink, which is not currently
  supported. Prefer the assembly path or Backend A.

## Recommendation

Prefer **Backend A** (`libpayload.so`): it needs no change to how `libapp.so` is
produced, is fully working and statically verified, and keeps the payload in its
own inspectable object. Backend B remains a valid option once the Flutter tool's
app link is hooked via the assembly path.
