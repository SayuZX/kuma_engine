# Flutter API demo

Create an Android application with the Flutter revision recorded in
`../engine_build_config.json`. Copy `demo_main.dart` to its `lib/main.dart`,
and copy the three files from `demo_assets/` to the app's `assets/` directory.
Declare `assets/` under `flutter.assets` in `pubspec.yaml`, keeping
`uses-material-design: true`.

The demo displays `Image.asset('assets/logo.png')` and loads text and JSON
concurrently through `rootBundle.load`. Its 1×1 PNG is an intentionally small
loader fixture, not a visual performance benchmark. The expected text is
`Hello from custom native payload! (3)`.

Use the versions and build steps in `../docs/android-packaging.md`; for the
verified host setup the Android app uses NDK 28.2.13676358 and build-tools
37.0.0. The standard Flutter template's release keystore is a test key and
must not be treated as a production signing identity.

Compilation, ELF inspection and synthetic APK tests do not establish that this
screen renders on Android. Install and launch the candidate with an ARM64 device
before marking the runtime milestone complete. The demo deliberately preserves
normal Dart asset path literals, which are currently rejected by the strict
plaintext release gate.
