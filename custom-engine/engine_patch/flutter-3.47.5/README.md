# Pinned Flutter 3.47.5 app engine

This patch reproduces the Android engine integration used by the KumaNime
Backend A APK prototype. Apply it to a clean Flutter framework checkout at
commit `6a19cca56475dbfba1478ee68d7bd0c2ef891da1`:

```bash
bash custom-engine/engine_patch/flutter-3.47.5/apply.sh /path/to/flutter
```

Run the command from the `kuma_engine` repository. The script checks the
framework commit and whether the patch applies before changing files. It
copies the native runtime sources and fixture into
`engine/src/flutter/assets/`, adds the packed release integration to Flutter's
Gradle plugin, applies the tracked integration edits, and generates a
public-key header from the repository's 32-byte Ed25519 public key. The
private signing key remains outside both repositories.
It also copies the Dart packer and ELF linker into the Flutter SDK so the
normal release build can find them without an app-side script.

Configure two Android release GN output directories with
`flutter_custom_asset_hardened = true`: `target_cpu = "arm64"` in
`out/android_release_arm64_lto`, and `target_cpu = "arm"` in
`out/android_release_lto`. Set `flutter_runtime_mode = "release"`,
`dart_runtime_mode = "release"`, `is_official_build = true`, and
`enable_lto = true` in both `args.gn` files. The pinned toolchain and other
build settings are recorded in `custom-engine/engine_build_config.json`.
From the matching `engine/src` checkout, regenerate and build with the actual
bundled GN and Ninja tools:

```bash
flutter/third_party/gn/gn gen out/android_release_arm64_lto
flutter/third_party/gn/gn gen out/android_release_lto
ninja -C out/android_release_arm64_lto -j8 libflutter.so
ninja -C out/android_release_lto -j8 libflutter.so
ninja -C out/host_debug_unopt asset_signer
```

The Flutter Gradle plugin selects those engine outputs when the application
sets `flutter.customPackedAssets=true` and runs `flutter build apk --release`.
It strips the `.so` files during native staging and checks their CPU, the
payload public-key header age, and hardened mode. This source patch is separate from the Flutter 3.27 engine port
in the root of `kuma_engine`; their revision-specific integration files must
not be mixed.
