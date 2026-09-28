#!/usr/bin/env python3
"""Measure signed payload size and full host verification, including process start."""

import argparse
import pathlib
import statistics
import struct
import subprocess
import tempfile
import time


def make_v2(path, count):
    blob_offset = 32 + 32 * count
    with path.open("wb") as output:
        output.write(struct.pack("<4sHHIIQQ", b"FEAP", 2, 0, count, 32, blob_offset, 0))
        for i in range(count):
            output.write(struct.pack("<QQIIII", i + 1, i, 1, 1, 0, 0))
        output.write(b"X" * count)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--signer", required=True, type=pathlib.Path)
    parser.add_argument("--repetitions", type=int, default=7)
    args = parser.parse_args()
    if args.repetitions < 3:
        parser.error("--repetitions must be at least 3")
    with tempfile.TemporaryDirectory(prefix="signed-index-bench-") as directory:
        root = pathlib.Path(directory)
        private, public = root / "key.ed25519-private", root / "key.ed25519-public"
        subprocess.run(
            [args.signer, "keygen", "--private", private, "--public", public],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        print("count v2_bytes v3_bytes index_median_ms index_min_ms index_max_ms full_median_ms full_min_ms full_max_ms")
        for count in (100, 1_000, 10_000, 50_000):
            source, signed = root / "source.bin", root / "signed.bin"
            make_v2(source, count)
            subprocess.run(
                [args.signer, "sign", "--input", source, "--output", signed, "--private", private],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            results = []
            for extra in (("--index-only",), ()):
                durations = []
                for _ in range(args.repetitions):
                    start = time.perf_counter()
                    subprocess.run(
                        [args.signer, "verify", "--input", signed, "--public", public, *extra],
                        check=True,
                        stdout=subprocess.DEVNULL,
                    )
                    durations.append((time.perf_counter() - start) * 1000)
                results.extend((statistics.median(durations), min(durations), max(durations)))
            print(
                count,
                source.stat().st_size,
                signed.stat().st_size,
                *(f"{value:.3f}" for value in results),
            )


if __name__ == "__main__":
    main()
