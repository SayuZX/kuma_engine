#!/usr/bin/env python3
"""Check that the loader can resolve both payload symbols from a shared library."""

import ctypes
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ENGINE_SRC = Path(os.environ["ENGINE_SRC"])


def main():
    source = ROOT / "linker/testdata/payload.bin"
    assembly = ROOT / "linker/payload_section.S"
    clang = ENGINE_SRC / "flutter/buildtools/mac-x64/clang/bin/clang"
    with tempfile.TemporaryDirectory(prefix="packed-dlopen-") as directory:
        library_path = Path(directory) / "libpayload.dylib"
        subprocess.run(
            [clang, "-dynamiclib", f'-DPAYLOAD_FILE="{source}"', assembly, "-o", library_path],
            check=True,
        )
        library = ctypes.CDLL(str(library_path))
        start = ctypes.addressof(ctypes.c_ubyte.in_dll(library, "__flutter_payload_start"))
        end = ctypes.addressof(ctypes.c_ubyte.in_dll(library, "__flutter_payload_end"))
        if end <= start or ctypes.string_at(start, end - start) != source.read_bytes():
            raise RuntimeError("loaded payload region differs from the source")
        print(f"native library symbols: {end - start} payload bytes match")


if __name__ == "__main__":
    main()
