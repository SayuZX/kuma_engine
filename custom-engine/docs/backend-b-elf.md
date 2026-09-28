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

## Real AOT relink milestone (Flutter 3.47 verification checkout)

`build_backend_b.py` now links assembly generated from an actual Flutter demo
kernel. It reads the snapshot symbol names from the chosen engine's
`runtime/dart_snapshot.cc`, verifies that the object defines them, and permits
an explicit linker alias when the assembly uses an extra leading underscore.
Every alias is reported in the adjacent JSON build report. Unknown layouts or
missing symbols fail before replacing an existing output.

The measured demo contains 782,533 bytes of signed payload; the stripped
`libapp.so` is 3,974,360 bytes and exports four symbols: two snapshot symbols
and the two payload bounds. This is **static ELF validation**, not a successful
Android launch. The 3.27 repository port and the device runtime remain untested.

```bash
ENGINE_SRC=/path/to/engine/src
"$ENGINE_SRC/out/android_release_arm64/clang_x64/gen_snapshot" \
  --deterministic --snapshot_kind=app-aot-assembly \
  --assembly=/path/to/app.S --strip /path/to/matching/app.dill
python3 custom-engine/linker/build_backend_b.py \
  --engine-src "$ENGINE_SRC" --assembly /path/to/app.S \
  --payload /path/to/payload.signed.bin --output /path/to/libapp.so
```

Use the kernel and snapshot options from the same release build being packaged.
Deferred loading is not integrated. The toolchain paths and NDK version currently
match the pinned macOS host in `engine_build_config.json`.

`payload_keep.ld` retains the section with `KEEP()` during garbage collection.
The export version script hides every symbol except the names the engine needs.
The Android Clang driver adds `--no-rosegment` for the API 24 target to preserve
old Android crash-unwinder compatibility. An initial experiment forced
`--rosegment`; review against the [NDK build guide](https://android.googlesource.com/platform/ndk/+/master/docs/BuildSystemMaintainers.md)
showed why that override was unsafe for compatibility, and it was removed.
The payload section is read-only, but shares an executable segment on this target;
the JSON report exposes this as `payload_segment_executable`. Backend A's separate
data-only library avoids that tradeoff. Requiring a non-executable segment for
Backend B needs a separately validated layout or a newer minimum Android version.
Validation checks actual file offsets and virtual addresses against a non-writable
`PT_LOAD`, byte equality,
16 KiB segment alignment, symbol bounds, no undefined/extra exports, and stripping.

`tests/backend_b_fixture.py` exercises repeatable linking and rejection of missing
snapshot symbols while preserving an earlier valid output. Payload authenticity
is checked separately by `asset_signer verify` and the APK release gate.

Backend A remains the simpler packaging path. Backend B now has a real AOT
linker, but packaging and an Android runtime test are still required before it
can be called a working app.
