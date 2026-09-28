# Android APK pipeline

The host-verified packaging stage takes a normal ARM64 release APK built with
the custom engine, extracts its final `assets/flutter_assets/` bundle, packs and
signs those exact bytes, and writes an unsigned candidate with one native payload.
This includes the manifests, shaders and font subsets produced by the actual
Flutter release build. Packing an earlier `flutter build bundle` output can ship
stale assets and is not sufficient for the final APK.

Backend A replaces any existing `libpayload.so`. Backend B replaces `libapp.so`
with the linked AOT-plus-payload library and removes `libpayload.so`. Both remove
the standard Flutter bundle and obsolete JAR signatures. The ZIP is reconstructed,
so any APK v2/v3 signature block is also discarded. Alignment and signing must
happen afterward. Native payload libraries are stored without ZIP compression.

`package_apk.py` rejects duplicate/noncanonical ZIP names, path traversal, missing
ARM64 app/engine libraries, other ABIs, and absent Flutter assets. It builds in a
temporary directory and replaces its output only after successful checks.
It never modifies the input APK. Intermediate APKs and unpacked assets are build
artifacts; they must not be copied into the distributed APK.

## Engine preparation

Use a synced checkout with the resolver patch and existing host tools, and record
its exact revision separately from the 3.27 source port in this repository.
Generate the Ed25519 key pair outside source control. Keep the private key in the
build environment; the hardened engine needs only the matching public header.

```bash
export ENGINE_SRC=/path/to/engine/src
export ASSET_PUBLIC_KEY=/secure/build/payload.ed25519-public
export ASSET_PRIVATE_KEY=/secure/build/payload.ed25519-private
export PATH=/path/to/depot_tools:$PATH
bash custom-engine/scripts/build_engine.sh
```

The script requires an existing `android_release_arm64/args.gn` with
`flutter_custom_asset_hardened = true`. It exports the public header and builds
the actual `flutter.jar` target with one Ninja job. This also refreshes
`arm64_v8a_release.jar`, which Flutter's local Maven repository uses. Relinking
only `libflutter.so` leaves those JARs stale. The script also builds the host
`const_finder.dart.snapshot` and `font-subset` tools used by icon tree shaking.

Expose NDK 28.2.13676358 under the configured Android SDK's `ndk/` directory
(a directory symlink to the existing engine NDK works). The inspected Flutter
3.47 provisioning code checks the SDK directory's installed-version list and
does not honor `ndkPath` when deciding to invoke the installer. Merely setting
`ndkPath` can therefore trigger a redundant NDK download.

## App build and candidate signing

`build_app.sh` supports a single, unflavored ARM64 release app. It caps Gradle at
one worker and a 2 GiB heap via process-local system properties; the project is
not edited. It uses the pinned macOS-host cross snapshot compiler. It does not
support deferred loading or extra snapshot compiler options yet.

```bash
export FLUTTER_ROOT=/path/to/flutter
export ANDROID_BUILD_TOOLS=/path/to/android-sdk/build-tools/37.0.0
export APK_KEYSTORE=/secure/build/app.keystore
export APK_KEY_ALIAS=your-alias
# Supply APK_KS_PASSWORD and APK_KEY_PASSWORD through the CI secret environment.
bash custom-engine/scripts/build_app.sh /path/to/app B
```

For Backend B, the wrapper matches the staged unstripped AOT library against
the Flutter build cache and obtains the kernel from that exact build directory.
It rejects a missing or ambiguous kernel instead of selecting a dill by timestamp.
It generates AOT assembly, repacks, aligns native ZIP entries at 16 KiB, signs,
checks the APK signature/alignment and invokes the structural/integrity release
gate. A failing gate returns nonzero and leaves `app-candidate.apk` for inspection;
it does not label it a validated release. Test signing keys are only suitable for
prototype installation.

For an already built APK, run just the packaging stage:

```bash
python3 custom-engine/scripts/package_apk.py \
  --input /path/to/app-release.apk --output /path/to/app-unsigned.apk \
  --backend B --assembly /path/to/matching/app.S \
  --engine-src "$ENGINE_SRC" \
  --dart "$FLUTTER_ROOT/bin/cache/dart-sdk/bin/dart" \
  --private "$ASSET_PRIVATE_KEY" --public "$ASSET_PUBLIC_KEY"
```

This output is unsigned. The shell wrapper contains the required `zipalign` and
`apksigner` order. `verify_release_apk.sh` by itself checks payload structure and
integrity; Android signature validity is checked separately with `apksigner`.

## Verification and remaining gates

`tests/package_apk_fixture.py` exercises both backends with synthetic ARM64 ELF
fixtures, byte-for-byte deterministic repacking, signature verification, removal
of standard assets and old signature files, and rejection of traversal entries.
These fixtures are not installable Android applications.

The real demo's relinked `libapp.so` still contains the literal names
`assets/logo.png`, `assets/message.txt` and `assets/data.json` from the Dart
call sites. Hashing the resolver index does not obfuscate application strings.
The strict plaintext gate will reject this demo; no allowlist or silent bypass
has been added. This is a separate outstanding requirement from the absence of
the `flutter_assets` directory or verification of the payload signature.

## Actual Backend B APK result

The Flutter 3.47 verification checkout produced a signed candidate on 2026-09-28:

| Item | Result |
| --- | --- |
| APK | 16,982,773 B; test signing identity |
| `libflutter.so` | 13,259,584 B; 70 defined dynamic symbols |
| `libapp.so` | 3,417,688 B; 4 defined dynamic symbols |
| Signed payload v3 | 225,877 B; 11 assets |
| Native library set | exactly `lib/arm64-v8a/libflutter.so` and `lib/arm64-v8a/libapp.so` |
| APK Flutter asset directory | absent |
| APK signatures | v2 and v3 verified |
| Native ZIP entry alignment | both stored uncompressed and aligned to 16 KiB |
| Payload signature and all block digests | passed |
| Strict plaintext gate | **failed**: the three Dart literals listed above |
| Install/launch and performance | unverified; no ADB device attached |

The adjacent `backend-b-apk-result.json` records hashes and inspection details.
The wrapper correctly returned exit status 1 when the plaintext check failed.
This candidate is not a release that passes every gate. The intermediate APK
used during packaging is not an upstream benchmark baseline.

Install/launch, `Image.asset`, font rendering, concurrent loads, cold/warm startup
and RSS still need a physical ARM64 device. Native compilation of the separate
3.27 repository port also remains unverified; this result uses the recorded 3.47
checkout. No claim is made that changing language or using handwritten assembly
would solve the remaining application-string or client-side extraction limits.
