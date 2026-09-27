# Backend A — embedding the payload in a native ELF section

Backend A ships the packed payload as its own shared object, `libpayload.so`,
alongside `libflutter.so` in `lib/arm64-v8a/`. It is the easiest backend to debug
because the payload is isolated and inspectable with standard ELF tools.

## Mechanism

`linker/payload_section.S` places `payload.bin` into a dedicated section and
brackets it with two global symbols:

```
  .section .flutter_payload,"a"      (ELF, allocatable read-only)
  .globl __flutter_payload_start
  .p2align 4
__flutter_payload_start:
  .incbin PAYLOAD_FILE
  .globl __flutter_payload_end
__flutter_payload_end:
```

On Apple hosts (used only for the host unit test) the same file targets a
Mach-O `__DATA,__flutterpay` section; the C symbol names are identical. The
section name differs only because Mach-O section names are capped at 16 chars.

## Symbol contract

The engine wiring in `android_shell_holder.cc` declares the two symbols weak:

```cpp
__attribute__((weak)) extern const uint8_t __flutter_payload_start[];
__attribute__((weak)) extern const uint8_t __flutter_payload_end[];
```

If no `libpayload.so` is loaded, both resolve to null and the resolver is not
added — behavior is identical to upstream. When present, the payload size is
`__flutter_payload_end - __flutter_payload_start`.

## Keeping the section

The symbols are exported through `linker/libpayload.ver`, which keeps the section
alive under `--gc-sections`. A `KEEP(*(.flutter_payload))` entry in a full linker
script is the equivalent when a `SECTIONS` script is already in use.

## Building and verifying (no device needed)

```bash
bash custom-engine/linker/build_libpayload.sh <payload.bin> <out/libpayload.so>
```

The script assembles for `aarch64-linux-android24` with the NDK sysroot, links
the shared object with the version script and `--gc-sections`, then verifies:

- `.flutter_payload` section present with the `A` (alloc) flag;
- `__flutter_payload_start` / `__flutter_payload_end` exported in `.dynsym`;
- `end - start` equals the payload byte size;
- the object is an AArch64 `DYN` shared object.

## Verified so far

- `libpayload.so` builds and passes all four static checks above.
- Host test `packed_payload_section_test` constructs the real
  `PackedAssetResolver` from the embedded section symbols and reads every asset
  back byte-identically (missing key → null), mirroring the Android wiring.

## Still open (needs a device / full engine relink)

- Load-order and global-scope symbol resolution so `libflutter.so`'s weak refs
  bind to `libpayload.so` at runtime on Android.
- Relinking `libflutter.so` and packaging an APK with `libpayload.so`, then
  confirming `rootBundle.load` / `Image.asset` on screen and `flutter_assets/`
  removed. Backend B (payload embedded directly in `libapp.so`) follows.
