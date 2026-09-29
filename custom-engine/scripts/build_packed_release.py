#!/usr/bin/env python3
"""Build and verify ARM64/ARMv7 release APKs with signed native assets.

The Flutter build makes ordinary AOT APKs first. This script converts them to
the custom engine's signed FEAP payload, then aligns and signs both APKs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from zipfile import ZipFile

from package_release_apk import inspect_archive, rewrite_unsigned_apk
from verify_release_apk import verify
from verify_packed_pair import verify_pair


ABIS = {
    "arm64-v8a": ("android_release_arm64", "arm64"),
    "armeabi-v7a": ("android_release", "arm"),
}
ASSET_PREFIX = "assets/flutter_assets/"


def _flutter_files(apk: Path) -> dict[str, bytes]:
    files = {}
    with ZipFile(apk) as archive:
        for info in archive.infolist():
            if not info.filename.startswith(ASSET_PREFIX) or info.is_dir():
                continue
            name = info.filename[len(ASSET_PREFIX):]
            parts = PurePosixPath(name).parts
            if not name or name.startswith("/") or ".." in parts or "\\" in name:
                raise ValueError(f"unsafe Flutter asset path: {name!r}")
            if name in files:
                raise ValueError(f"duplicate Flutter asset: {name}")
            files[name] = archive.read(info)
    if not files:
        raise ValueError(f"no Flutter assets in input APK: {apk}")
    return files


def extract_matching_assets(apks: list[Path], destination: Path) -> list[str]:
    """Extract assets only if every ABI APK contains identical Flutter bytes."""
    if not apks:
        raise ValueError("no APKs to compare")
    reference = _flutter_files(apks[0])
    for apk in apks[1:]:
        other = _flutter_files(apk)
        if other != reference:
            changed = sorted(
                name for name in set(reference) | set(other)
                if reference.get(name) != other.get(name)
            )
            raise ValueError(f"different Flutter asset content across ABIs: {changed[:5]}")
    for name, data in reference.items():
        path = destination.joinpath(*PurePosixPath(name).parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return sorted(reference)


def _run(*command: str, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    subprocess.run([str(piece) for piece in command], cwd=cwd, env=env, check=True)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha1(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _properties(path: Path) -> dict[str, str]:
    result = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "!")):
            continue
        if "=" not in line:
            raise ValueError(f"invalid property in {path.name}")
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def _engine_ready(out: Path, cpu: str, expected_header: bytes) -> Path:
    args = (out / "args.gn").read_text(encoding="utf-8")
    if f'target_cpu = "{cpu}"' not in args:
        raise ValueError(f"engine CPU mismatch: {out}")
    if 'flutter_runtime_mode = "release"' not in args:
        raise ValueError(f"engine is not release: {out}")
    if "flutter_custom_asset_hardened = true" not in args:
        raise ValueError(f"engine lacks signed-only resolver: {out}")
    library = out / "libflutter.so"
    if not library.is_file():
        raise FileNotFoundError(library)
    if library.stat().st_mtime < (out.parents[1] / "flutter/assets/packed_asset_public_key_generated.h").stat().st_mtime:
        raise ValueError(f"engine predates current public key: {out}")
    if not expected_header:
        raise ValueError("missing expected public key header")
    return library


def _build(args: argparse.Namespace) -> None:
    app = args.app.resolve()
    flutter_root = args.flutter_root.resolve()
    custom = Path(__file__).resolve().parents[1]
    engine_src = args.engine_src.resolve() if args.engine_src else flutter_root / "engine/src"
    dart = flutter_root / "bin/cache/dart-sdk/bin/dart"
    signer = engine_src / "out/host_debug_unopt/asset_signer"
    toolchain = engine_src / "flutter/buildtools/mac-x64/clang/bin"
    strip = toolchain / "llvm-strip"
    readelf = toolchain / "llvm-readelf"
    key_dir = args.key_dir.expanduser().resolve()
    private_key = key_dir / "payload-private.ed25519"
    public_key = key_dir / "payload-public.ed25519"
    header = engine_src / "flutter/assets/packed_asset_public_key_generated.h"
    properties = _properties(app / "android/key.properties")
    for key in ("storeFile", "storePassword", "keyAlias", "keyPassword"):
        if not properties.get(key):
            raise ValueError(f"android/key.properties lacks {key}")
    release_keystore = (app / "android/app" / properties["storeFile"]).resolve()
    sdk = Path(_properties(app / "android/local.properties")["sdk.dir"])
    build_tools = sdk / "build-tools/35.0.0"
    zipalign = build_tools / "zipalign"
    apksigner = build_tools / "apksigner"
    for required in (dart, signer, strip, readelf, private_key, public_key,
                     release_keystore, zipalign, apksigner, header):
        if not required.is_file():
            raise FileNotFoundError(required)

    with tempfile.TemporaryDirectory(prefix="kuma-packed-release-") as temp_name:
        temp = Path(temp_name)
        generated_header = temp / "key.h"
        _run(signer, "export-header", "--public", public_key,
             "--output", generated_header)
        if generated_header.read_bytes() != header.read_bytes():
            raise ValueError("public key differs from the custom engine header")
        engines = {
            abi: _engine_ready(engine_src / "out" / out, cpu, generated_header.read_bytes())
            for abi, (out, cpu) in ABIS.items()
        }

        if not args.skip_flutter_build:
            command = [str(flutter_root / "bin/flutter"), "build", "apk", "--release",
                       "--target-platform", "android-arm,android-arm64", "--split-per-abi"]
            if args.verbose:
                command.append("-v")
            _run(*command, cwd=app)

        apk_dir = app / "build/app/outputs/flutter-apk"
        sources = {
            abi: apk_dir / f"app-{abi}-release.apk" for abi in ABIS
        }
        for source in sources.values():
            if not source.is_file():
                raise FileNotFoundError(source)

        asset_dir = temp / "flutter_assets"
        asset_names = extract_matching_assets(list(sources.values()), asset_dir)
        payload_dir = temp / "packed"
        _run(dart, custom / "asset_packer/flutter_asset_packer.dart",
             "--input", asset_dir, "--output", payload_dir,
             "--compression", "auto", "--hash", "fnv1a", "--alignment", "16")
        signed_payload = payload_dir / "payload-signed.bin"
        _run(signer, "sign", "--input", payload_dir / "payload.bin",
             "--output", signed_payload, "--private", private_key,
             "--alignment", "16")
        _run(signer, "verify", "--input", signed_payload, "--public", public_key)

        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        signed_outputs = {}
        report = {"asset_count": len(asset_names),
                  "signed_payload_bytes": signed_payload.stat().st_size,
                  "payload_sha256": _sha256(signed_payload), "apks": {}}
        sign_env = os.environ.copy()
        sign_env["KUMA_APK_STORE_PASSWORD"] = properties["storePassword"]
        sign_env["KUMA_APK_KEY_PASSWORD"] = properties["keyPassword"]

        for abi, source in sources.items():
            libpayload = temp / abi / "libpayload.so"
            linker_env = os.environ.copy()
            linker_env["ENGINE_SRC"] = str(engine_src)
            _run("bash", custom / "linker/build_libpayload.sh",
                 signed_payload, libpayload, abi, env=linker_env)
            stripped = temp / abi / "libflutter.so"
            _run(strip, "--strip-unneeded", "-o", stripped, engines[abi])
            dyn_symbols = subprocess.check_output([str(readelf), "--dyn-syms", str(stripped)], text=True)
            if "JNI_OnLoad" not in dyn_symbols:
                raise ValueError(f"JNI_OnLoad missing after stripping {abi} engine")
            sections = subprocess.check_output([str(readelf), "-S", str(stripped)], text=True)
            if ".debug_info" in sections:
                raise ValueError(f"debug sections remain in {abi} engine")

            unsigned = temp / abi / "unsigned.apk"
            aligned = temp / abi / "aligned.apk"
            destination = output_dir / source.name
            rewrite_unsigned_apk(source, unsigned, abi, stripped, libpayload)
            _run(zipalign, "-P", "16", "-f", "4", unsigned, aligned)
            _run(apksigner, "sign", "--ks", release_keystore,
                 "--ks-key-alias", properties["keyAlias"],
                 "--ks-pass", "env:KUMA_APK_STORE_PASSWORD",
                 "--key-pass", "env:KUMA_APK_KEY_PASSWORD",
                 "--out", destination, aligned, env=sign_env)
            _run(apksigner, "verify", "--verbose", destination)
            _run(zipalign, "-c", "-P", "16", "4", destination)
            inspect_archive(destination, abi)
            inspected = verify(destination, engine_src, public_key, build_tools)
            signed_outputs[abi] = destination
            report["apks"][abi] = {
                "source_bytes": source.stat().st_size,
                "packed_bytes": destination.stat().st_size,
                "saving_bytes": source.stat().st_size - destination.stat().st_size,
                "sha256": _sha256(destination),
                "dart_plaintext_path_count": inspected["dart_plaintext_path_count"],
            }

        report_json = json.dumps(report, indent=2, sort_keys=True) + "\n"
        (output_dir / "packed-release-report.json").write_text(
            report_json, encoding="utf-8")

        if args.replace_standard:
            for abi, destination in signed_outputs.items():
                source = sources[abi]
                staged = source.with_suffix(".packed.tmp")
                shutil.copyfile(destination, staged)
                staged.replace(source)
                (apk_dir / f"{source.name}.sha1").write_text(
                    _sha1(source), encoding="ascii")
                gradle_apk = app / "build/app/outputs/apk/release" / source.name
                if gradle_apk.is_file():
                    shutil.copy2(source, gradle_apk)
                verify(source, engine_src, public_key, build_tools)
            for generated_root in (apk_dir, app / "build/app/outputs/apk/release"):
                for stale_name in ("app-release.apk", "app-release.apk.sha1"):
                    stale = generated_root / stale_name
                    if stale.is_file():
                        stale.unlink()

            verify_pair(app, engine_src, public_key, build_tools)

            (app / "build/app/outputs/packed-release-report.json").write_text(
                report_json, encoding="utf-8")

        print(json.dumps(report, indent=2, sort_keys=True))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--flutter-root", required=True, type=Path)
    parser.add_argument("--engine-src", type=Path,
                        help="engine checkout root (defaults to <flutter-root>/engine/src)")
    parser.add_argument("--key-dir", type=Path, default=Path("~/.config/kuma-engine"))
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--skip-flutter-build", action="store_true")
    parser.add_argument("--replace-standard", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    try:
        _build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"packed release failed: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
