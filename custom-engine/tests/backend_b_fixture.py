#!/usr/bin/env python3
"""Exercise Backend B ELF layout and failure behavior; not a Dart runtime test."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE_SRC = Path(os.environ['ENGINE_SRC'])


def main():
    source = (ENGINE_SRC / 'flutter/runtime/dart_snapshot.cc').read_text()
    names = sorted(set(re.findall(
        r'const char\* DartSnapshot::k(?:VM|Isolate)(?:Data|Instructions)Symbol\s*=\s*"([A-Za-z0-9_]+)";',
        source)))
    with tempfile.TemporaryDirectory(prefix='backend-b-test-') as directory:
        work = Path(directory)
        assembly = work / 'fixture.S'
        assembly.write_text('\n'.join(
            f'.section .rodata\n.globl _{name}\n_{name}:\n.quad 0' for name in names))
        output = work / 'libapp.so'
        command = [sys.executable, str(ROOT / 'linker/build_backend_b.py'),
                   '--engine-src', str(ENGINE_SRC), '--assembly', str(assembly),
                   '--payload', str(ROOT / 'linker/testdata/payload.bin'),
                   '--output', str(output)]
        subprocess.run(command, check=True, capture_output=True)
        first = output.read_bytes()
        subprocess.run(command, check=True, capture_output=True)
        if output.read_bytes() != first:
            raise RuntimeError('repeated link is not deterministic')
        print('Backend B: deterministic ELF, payload bytes, PT_LOAD, exports, strip and 16 KiB alignment PASS')
        assembly.write_text('.text\n.globl unrelated\nunrelated:\nret\n')
        failed = subprocess.run(command, capture_output=True, text=True)
        if failed.returncode == 0 or 'does not define' not in failed.stderr:
            raise RuntimeError('missing Dart snapshot symbols were not rejected')
        if output.read_bytes() != first:
            raise RuntimeError('failed build replaced the previous library')
        print('Backend B: missing snapshot symbols rejected; previous output preserved PASS')


if __name__ == '__main__':
    main()
