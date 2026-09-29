# KumaNime dual-ABI packed release

This is the app-specific Backend A workflow for `arm64-v8a` and
`armeabi-v7a`. It was exercised with the separate Flutter 3.47.5 engine
checkout recorded in `engine_build_config.json`. The engine code in this
repository is based on Flutter 3.27; a native build of that port is still
unverified. Backend B remains an ARM64 experiment.

## Inputs and release command

The app checkout must have an Android release signing configuration in
`android/key.properties`. Keep the Ed25519 private payload key outside both
repositories at `~/.config/kuma-engine/payload-private.ed25519` (mode 0600);
the matching public key is in `custom-engine/keys/`. Generate
`flutter/assets/packed_asset_public_key_generated.h` from this public key and
relink **both** hardened release engines before packaging. Both `args.gn`
files must set `flutter_custom_asset_hardened = true` and target the correct
CPU. The script rejects a library older than the generated public header.

For the inspected KumaNime app revision, the app-specific text edits and
checksum-checked removal of the two unused Dolby files are reproducible with:

```bash
bash custom-engine/app_patch/apply_kumaanime.sh /path/to/KumaAnime-App
```

The patch requires the inspected target lines and refuses changed Dolby bytes.
It contains no proprietary APK bytes or signing secrets. From the KumaNime
app checkout afterward:

```bash
flutter pub get
bash tool/build_packed_release.sh -v
```

The wrapper runs `build_packed_release.py --replace-standard`. A direct
`flutter build apk --release` in the app is guarded at `preReleaseBuild` and
fails: an ordinary Flutter build would package `assets/flutter_assets/`.
The PowerShell release wrapper must also fail until a Windows-host engine
pipeline exists. Remove the two inert Dolby APKs from
`android/app/src/main/assets/dolby/`; the app's native Audio FX code does not
consume them. Keep the sealed emoji font payload because the app loads it.

The final outputs are:

```text
build/app/outputs/flutter-apk/app-arm64-v8a-release.apk
build/app/outputs/flutter-apk/app-armeabi-v7a-release.apk
build/app/outputs/packed-release-report.json
```

The wrapper deletes a stale universal `app-release.apk` in both Flutter and
Gradle output directories, so a previous non-packed artifact cannot be
mistaken for the new build. It replaces split APKs only after both candidates
pass verification. The temporary stock APKs are build inputs, not release
artifacts.

For a packaging-only repeat with already built split APKs:

```bash
python3 custom-engine/scripts/build_packed_release.py \
  --app /path/to/KumaAnime-App \
  --flutter-root /path/to/flutter \
  --engine-src /path/to/flutter/engine/src \
  --output-dir /tmp/kuma-packed-output \
  --skip-flutter-build
```

After a release build, run the same two-APK gate independently in CI:

```bash
python3 custom-engine/scripts/verify_packed_pair.py \
  --app /path/to/KumaAnime-App \
  --engine-src /path/to/flutter/engine/src
```

The build wrapper also runs this pair gate before writing its final report.
It rejects a stale universal APK and rejects differing payloads between ABIs.

The packer extracts the *exact* final Flutter assets from both stock APKs,
rejects ABI drift, packs them once, signs format v3, and links the same
payload bytes into each ABI's `libpayload.so`. It replaces `libflutter.so`
with the matching hardened library, omits Flutter assets and Dolby source
assets, aligns native libraries at 16 KiB, signs each APK with the app release
key, and reopens each archive for checks. The gate verifies ZIP contents,
ABI/ELF machine, APK signature, alignment, native debug sections, payload
section and symbols, the Ed25519 index signature, and every stored block
digest. It rejects plaintext asset paths in `libpayload.so` and
`libflutter.so`.

## Measured prototype, 29 September 2026

The first packaging-only run used stock app APKs created before the source
Dolby removal. The input-to-output comparison therefore includes removal of
the two Dolby APKs as well as the payload migration and custom engine.

| ABI | Input bytes | Packed bytes | Saving |
| --- | ---: | ---: | ---: |
| arm64-v8a | 70,257,061 | 64,200,539 | 6,056,522 (8.62%) |
| armeabi-v7a | 63,356,445 | 56,156,009 | 7,200,436 (11.36%) |

Both contain 35 logical Flutter assets in one 11,000,735-byte signed
payload. Every asset was read byte-for-byte using the host native resolver.
Both APKs passed signature, ZIP alignment, ABI, payload signature and digest
checks. An ARM64-only and an ARMv7-only APK were installed on a physical
Android 12 device using a temporary **debug** signature matching the test
installation. Both launched and rendered Home; ARM64 also rendered Settings.
The ARMv7 installation reported `primaryCpuAbi=armeabi-v7a`. The original
debug APK was restored after each test. The distributable candidates remain
signed with the configured release key, not the device test key.

The packed payload is approximately the same size as the stock ZIP-compressed
Flutter asset directory. Most APK savings here came from removing 7.70 MB of
compressed, unused Dolby APKs; packing alone is not a meaningful size win for
these particular assets. The WebRTC library and sealed emoji font remain
because the app uses them. No startup/RSS comparison or long navigation test
has been measured yet.

The subsequent **fresh source build**, after removing Dolby from the source
tree, provides a cleaner size comparison for the packing step itself:

| ABI | Fresh stock APK without Dolby | Packed APK | Packing overhead |
| --- | ---: | ---: | ---: |
| arm64-v8a | 65,308,427 B | 66,953,051 B | 1,644,624 B (2.52%) |
| armeabi-v7a | 58,948,487 B | 59,449,193 B | 500,706 B (0.85%) |

Both packed APKs passed the two-ABI release gate. The stock APKs in this table
were the exact inputs to that run, so app-code drift does not affect this
comparison. The larger custom engine (built without LTO), not the small
hashed index, accounts for most of the overhead. LTO and size-tuned native
linking are the next measurements; no unsafe subsystem deletion is assumed.

| Engine `libflutter.so` | Stock stripped | Custom stripped | Difference |
| --- | ---: | ---: | ---: |
| arm64-v8a | 11,747,864 B | 13,259,584 B | +1,511,720 B |
| armeabi-v7a | 8,615,900 B | 8,970,996 B | +355,096 B |

The 35-asset corpus was also compressed independently per asset on the macOS
host, applying the current auto rule (skip known internally compressed file
types; accept a candidate only if it saves at least 64 bytes and 5%). These
figures exclude index, signatures and alignment bytes:

| Codec | Stored asset bytes | Difference from zlib |
| --- | ---: | ---: |
| None | 14,646,392 | +3,683,915 |
| zlib level 6 | 10,962,477 | baseline |
| zstd level 3 | 10,819,681 | -142,796 |
| LZ4 default | 11,672,352 | +709,875 |

The CLI comparison used Zstandard 1.5.7 and LZ4 1.10.0. It measures size,
not Android decoder latency or binary dependency cost. Zstandard's gain is
too small to cover the current engine-size gap on its own; the runtime
therefore remains on the already tested zlib codec rather than adding another
decoder without a measured overall win.

## Security and compatibility limits

The hashed index contains no plaintext asset-name table. The AOT application
library still contains 20 asset-path literals compiled from Dart call sites in
this prototype.
The existing strict `verify_release_apk.sh` rejects those literals; this
app-specific Python gate reports their count and accepts them because the
Flutter `Image.asset` / `rootBundle` API still passes string keys. This is an
explicit exception to the strict gate, not proof of hidden application strings.
Flutter's required `AssetManifest.bin` and font manifest can also reveal
logical names after decoding, even though their stored bytes are packed.
An offline client cannot keep its decryption/signing logic secret from a
determined reverse engineer. The private signing key is build-only; the
runtime embeds only the public key. This APK workflow authenticates payloads
but does not enable the optional AEAD encryption module, so it makes no
confidentiality claim.

Backend A adds `libpayload.so`. The two-library final APK target requires a
separate, device-verified Backend B link for each ABI and has not been reached
by this workflow. The app's Java embedding was produced by the stock Flutter
build, then the matching-revision custom `libflutter.so` was substituted.
Both tested ABIs launched, but pinned custom-engine Java artifacts are still
the preferred production build path.
