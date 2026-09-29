"""Rewrite a split release APK for the packed Flutter asset resolver.

This module produces an unsigned APK. The release build pipeline must align,
sign, and verify it before publishing the result.
"""

from __future__ import annotations

from pathlib import Path, PurePosixPath
from zipfile import ZIP_STORED, ZipFile


def _native_abis(names: list[str]) -> set[str]:
    return {parts[1] for name in names if (parts := name.split("/"))[:1] == ["lib"] and len(parts) >= 3}


def _validate_names(names: list[str]) -> None:
    if len(set(names)) != len(names):
        raise ValueError("duplicate APK entry names")
    for name in names:
        path = PurePosixPath(name)
        if (not name or path.is_absolute() or ".." in path.parts or
                "\\" in name or "\0" in name or str(path) != name.rstrip("/")):
            raise ValueError(f"unsafe ZIP path: {name!r}")


def _old_signature(name: str) -> bool:
    if not name.startswith("META-INF/"):
        return False
    leaf = name.removeprefix("META-INF/").upper()
    return leaf == "MANIFEST.MF" or leaf.endswith((".SF", ".RSA", ".DSA", ".EC"))


def inspect_archive(apk: Path, abi: str) -> None:
    """Raise if a release APK still has stock assets or the wrong native libs."""
    with ZipFile(apk) as archive:
        names = archive.namelist()
        _validate_names(names)
        name_set = set(names)
        if any(name.startswith("assets/flutter_assets/") for name in names):
            raise ValueError("assets/flutter_assets/ remains in APK")
        if any(name.startswith("assets/dolby/") for name in names):
            raise ValueError("assets/dolby/ remains in APK")
        if any(name.lower().endswith(".apk") for name in names):
            raise ValueError("nested APK remains in release archive")
        if any(name.endswith(".symbols") for name in names):
            raise ValueError("Dart symbol archive remains in APK")
        actual_abis = _native_abis(names)
        if actual_abis != {abi}:
            raise ValueError(f"APK ABI mismatch: expected {abi}, got {sorted(actual_abis)}")
        for library in ("libflutter.so", "libapp.so", "libpayload.so"):
            path = f"lib/{abi}/{library}"
            if path not in name_set:
                raise ValueError(f"missing {path}")
            if archive.getinfo(path).compress_type != ZIP_STORED:
                raise ValueError(f"native library compressed: {path}")


def rewrite_unsigned_apk(
    source: Path, output: Path, abi: str, engine: Path, payload: Path
) -> None:
    """Create an unsigned APK with a matching engine and native payload."""
    source, output, engine, payload = map(Path, (source, output, engine, payload))
    if source.resolve() == output.resolve():
        raise ValueError("input and output APK must differ")
    if abi not in ("arm64-v8a", "armeabi-v7a"):
        raise ValueError(f"unsupported ABI: {abi}")
    for path in (source, engine, payload):
        if not path.is_file():
            raise FileNotFoundError(path)

    engine_name = f"lib/{abi}/libflutter.so"
    payload_name = f"lib/{abi}/libpayload.so"
    with ZipFile(source) as original:
        names = original.namelist()
        _validate_names(names)
        actual_abis = _native_abis(names)
        if actual_abis != {abi}:
            raise ValueError(f"APK ABI mismatch: expected {abi}, got {sorted(actual_abis)}")
        if engine_name not in names or f"lib/{abi}/libapp.so" not in names:
            raise ValueError("input APK lacks Flutter native libraries")

        output.parent.mkdir(parents=True, exist_ok=True)
        with ZipFile(output, "w", allowZip64=True) as rewritten:
            rewritten.comment = original.comment
            for info in original.infolist():
                name = info.filename
                if name.startswith(("assets/flutter_assets/", "assets/dolby/")):
                    continue
                if name in (engine_name, payload_name) or _old_signature(name):
                    continue
                rewritten.writestr(info, original.read(info))
            rewritten.write(engine, engine_name, compress_type=ZIP_STORED)
            rewritten.write(payload, payload_name, compress_type=ZIP_STORED)

    inspect_archive(output, abi)
