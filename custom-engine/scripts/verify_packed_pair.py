#!/usr/bin/env python3
"""Release CI gate for the pair of KumaNime split packed APKs."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from verify_release_apk import verify


def verify_pair(app: Path, engine_src: Path, public_key: Path, build_tools: Path) -> dict:
    apk_dir = app / "build/app/outputs/flutter-apk"
    if (apk_dir / "app-release.apk").exists():
        raise ValueError("stale universal app-release.apk remains in Flutter outputs")
    if (app / "build/app/outputs/apk/release/app-release.apk").exists():
        raise ValueError("stale universal app-release.apk remains in Gradle outputs")

    results = {}
    for abi in ("arm64-v8a", "armeabi-v7a"):
        apk = apk_dir / f"app-{abi}-release.apk"
        results[abi] = verify(apk, engine_src, public_key, build_tools)
    digests = {result["payload_sha256"] for result in results.values()}
    if len(digests) != 1:
        raise ValueError("ARM64 and ARMv7 APKs embed different Flutter payloads")
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--engine-src", required=True, type=Path)
    parser.add_argument("--public-key", type=Path,
                        default=Path(__file__).resolve().parents[1] / "keys/payload-public.ed25519")
    parser.add_argument("--android-build-tools", type=Path,
                        default=Path(os.environ.get("ANDROID_BUILD_TOOLS",
                            "/usr/local/share/android-commandlinetools/build-tools/35.0.0")))
    args = parser.parse_args()
    try:
        results = verify_pair(args.app.resolve(), args.engine_src.resolve(),
                              args.public_key.resolve(), args.android_build_tools.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"PAIR VERIFY FAIL: {error}", file=sys.stderr)
        sys.exit(1)
    print("PAIR VERIFY PASS: " + ", ".join(
        f"{abi} {result['apk_bytes']} B" for abi, result in results.items()))


if __name__ == "__main__":
    main()
