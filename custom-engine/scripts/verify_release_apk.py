#!/usr/bin/env python3
"""Verify the final signed ARM64 or ARMv7 packed release APK."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from zipfile import ZipFile

from package_release_apk import inspect_archive


MACHINE = {"arm64-v8a": "AArch64", "armeabi-v7a": "ARM"}
ASSET_PATH = re.compile(rb"(?:assets|lib/assets)/[A-Za-z0-9_./-]+\.(?:png|jpg|jpeg|webp|json|ttf|otf|mp3|mp4)")


def _run(*args: Path | str) -> str:
    command = [str(arg) for arg in args]
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)


def verify(apk: Path, engine_src: Path, public_key: Path,
           build_tools: Path) -> dict[str, object]:
    if not apk.is_file():
        raise FileNotFoundError(apk)
    if not public_key.is_file():
        raise FileNotFoundError(public_key)
    with ZipFile(apk) as archive:
        names = archive.namelist()
        abis = {name.split("/")[1] for name in names if name.startswith("lib/") and len(name.split("/")) >= 3}
        if len(abis) != 1 or next(iter(abis)) not in MACHINE:
            raise ValueError(f"unsupported APK ABIs: {sorted(abis)}")
        abi = next(iter(abis))
        inspect_archive(apk, abi)
        for required in ("AndroidManifest.xml", "classes.dex", "resources.arsc"):
            if required not in names:
                raise ValueError(f"missing {required}")

        llvm = engine_src / "flutter/buildtools/mac-x64/clang/bin"
        signer = engine_src / "out/host_debug_unopt/asset_signer"
        tools = [llvm / "llvm-readelf", llvm / "llvm-objcopy",
                 build_tools / "apksigner", build_tools / "zipalign", signer]
        for tool in tools:
            if not tool.is_file():
                raise FileNotFoundError(tool)

        signature = _run(build_tools / "apksigner", "verify", "--verbose", apk)
        if "Verified using v2 scheme (APK Signature Scheme v2): true" not in signature:
            raise ValueError("APK v2 signature missing")
        _run(build_tools / "zipalign", "-c", "-P", "16", "4", apk)

        with tempfile.TemporaryDirectory(prefix="kuma-apk-verify-") as temp_name:
            temp = Path(temp_name)
            for library in ("libflutter.so", "libapp.so", "libpayload.so"):
                (temp / library).write_bytes(archive.read(f"lib/{abi}/{library}"))
                header = _run(llvm / "llvm-readelf", "-h", temp / library)
                if not re.search(rf"Machine:\s+{MACHINE[abi]}\b", header):
                    raise ValueError(f"wrong ELF machine: {library}")

            for library in ("libflutter.so", "libapp.so"):
                sections = _run(llvm / "llvm-readelf", "-S", temp / library)
                if ".debug_info" in sections:
                    raise ValueError(f"debug information remains in {library}")
            symbols = _run(llvm / "llvm-readelf", "--dyn-syms", temp / "libpayload.so")
            if "__flutter_payload_start" not in symbols or "__flutter_payload_end" not in symbols:
                raise ValueError("native payload linker symbols missing")
            if ASSET_PATH.search((temp / "libpayload.so").read_bytes()):
                raise ValueError("plaintext asset path found in native payload")
            if ASSET_PATH.search((temp / "libflutter.so").read_bytes()):
                raise ValueError("plaintext asset path found in Flutter engine")
            dart_literal_count = len(set(ASSET_PATH.findall((temp / "libapp.so").read_bytes())))

            payload = temp / "payload.bin"
            _run(llvm / "llvm-objcopy", "--dump-section",
                 f".flutter_payload={payload}", temp / "libpayload.so")
            if not payload.is_file() or not payload.stat().st_size:
                raise ValueError("native ELF payload section missing")
            _run(signer, "verify", "--input", payload, "--public", public_key)
            return {"abi": abi, "apk_bytes": apk.stat().st_size,
                    "payload_bytes": payload.stat().st_size,
                    "v2_signed": True, "payload_authenticated": True,
                    "dart_plaintext_path_count": dart_literal_count}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--engine-src", type=Path,
                        default=Path(os.environ.get(
                            "ENGINE_SRC", Path(__file__).resolve().parents[2] / "engine/src")))
    parser.add_argument("--android-build-tools", type=Path,
                        default=Path(os.environ.get(
                            "ANDROID_BUILD_TOOLS",
                            "/usr/local/share/android-commandlinetools/build-tools/35.0.0")))
    parser.add_argument("--public-key", type=Path,
                        default=Path(__file__).resolve().parents[1] / "keys/payload-public.ed25519")
    args = parser.parse_args()
    try:
        result = verify(args.apk.resolve(), args.engine_src.resolve(),
                        args.public_key.resolve(), args.android_build_tools.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"VERIFY FAIL: {error}", file=sys.stderr)
        sys.exit(1)
    print(f"VERIFY PASS: {result['abi']}, APK {result['apk_bytes']} B, "
          f"signed payload {result['payload_bytes']} B, "
          f"Dart asset literals {result['dart_plaintext_path_count']}")


if __name__ == "__main__":
    main()
