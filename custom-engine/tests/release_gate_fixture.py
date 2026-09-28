#!/usr/bin/env python3
"""Exercise the release APK gate with small synthetic AArch64 ELF fixtures."""

import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
ENGINE_SRC = Path(os.environ["ENGINE_SRC"])
TOOLS = ENGINE_SRC / "flutter/buildtools/mac-x64/clang/bin"
SIGNER = Path(os.environ.get("ASSET_SIGNER", ENGINE_SRC / "out/host_debug_unopt/asset_signer"))
SYSROOT = (
    ENGINE_SRC
    / "flutter/third_party/android_tools/sdk/ndk/28.2.13676358"
    / "toolchains/llvm/prebuilt/darwin-x86_64/sysroot"
)


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def make_apk(path, flutter, app, payload, extra_asset=False):
    with zipfile.ZipFile(path, "w") as apk:
        apk.writestr("AndroidManifest.xml", b"fixture")
        apk.write(flutter, "lib/arm64-v8a/libflutter.so")
        apk.write(app, "lib/arm64-v8a/libapp.so")
        apk.write(payload, "lib/arm64-v8a/libpayload.so")
        if extra_asset:
            apk.writestr("assets/flutter_assets/assets/hello.txt", b"duplicate")


def main():
    env = dict(
        os.environ,
        ENGINE_SRC=str(ENGINE_SRC),
        LLVM_READELF=str(TOOLS / "llvm-readelf"),
        LLVM_OBJCOPY=str(TOOLS / "llvm-objcopy"),
        ASSET_SIGNER=str(SIGNER),
    )
    with tempfile.TemporaryDirectory(prefix="packed-apk-gate-") as directory:
        work = Path(directory)
        private = work / "fixture.ed25519-private"
        public = work / "fixture.ed25519-public"
        source = ROOT / "linker/testdata/payload.bin"
        signed = work / "signed.bin"
        run(SIGNER, "keygen", "--private", private, "--public", public)
        run(SIGNER, "sign", "--input", source, "--output", signed, "--private", private)

        tampered = work / "tampered.bin"
        changed = bytearray(signed.read_bytes())
        changed[-1] ^= 1
        tampered.write_bytes(changed)

        stub = work / "stub.c"
        stub.write_text("int fixture(void) { return 0; }\n")
        app, flutter = work / "libapp.so", work / "libflutter.so"
        for library in (app, flutter):
            run(
                TOOLS / "clang",
                "--target=aarch64-linux-android24",
                f"--sysroot={SYSROOT}",
                "-shared",
                "-nostdlib",
                stub,
                "-o",
                library,
            )
            run(TOOLS / "llvm-strip", "--strip-unneeded", library)

        for label, blob, duplicate, expected in (
            ("valid", signed, False, 0),
            ("tampered", tampered, False, 1),
            ("duplicate-assets", signed, True, 1),
        ):
            payload = work / f"{label}.libpayload.so"
            run(
                "bash",
                ROOT / "linker/build_libpayload.sh",
                blob,
                payload,
                env=env,
                stdout=subprocess.DEVNULL,
            )
            apk = work / f"{label}.apk"
            make_apk(apk, flutter, app, payload, duplicate)
            result = subprocess.run(
                ["bash", str(ROOT / "scripts/verify_release_apk.sh"), str(apk), "--public", str(public)],
                env=env,
                text=True,
                capture_output=True,
                check=False,
            )
            actual = 0 if result.returncode == 0 else 1
            if actual != expected:
                print(result.stdout, result.stderr)
                raise RuntimeError(f"{label}: expected {expected}, got {result.returncode}")
            print(f"{label}: {'PASS' if actual == 0 else 'rejected as expected'}")


if __name__ == "__main__":
    main()
