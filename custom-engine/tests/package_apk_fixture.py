#!/usr/bin/env python3
"""Test unsigned APK assembly, both native backends, and malformed ZIP input."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

import release_gate_fixture as fixture

ROOT = fixture.ROOT


def checked(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def main():
    dart = Path(os.environ['DART'])
    env = dict(os.environ, LLVM_READELF=str(fixture.TOOLS / 'llvm-readelf'),
               LLVM_OBJCOPY=str(fixture.TOOLS / 'llvm-objcopy'),
               ASSET_SIGNER=str(fixture.SIGNER))
    with tempfile.TemporaryDirectory(prefix='package-apk-test-') as directory:
        work = Path(directory)
        private, public = work / 'private', work / 'public'
        fixture.run(fixture.SIGNER, 'keygen', '--private', private, '--public', public)
        source = (fixture.ENGINE_SRC / 'flutter/runtime/dart_snapshot.cc').read_text()
        names = sorted(set(re.findall(
            r'const char\* DartSnapshot::k(?:VM|Isolate)(?:Data|Instructions)Symbol\s*=\s*"([A-Za-z0-9_]+)";',
            source)))
        assembly = work / 'fixture.S'
        assembly.write_text('\n'.join(
            f'.section .rodata\n.globl _{name}\n_{name}:\n.quad 0' for name in names))
        stub = work / 'stub.c'
        stub.write_text('int fixture(void) { return 0; }\n')
        library = work / 'stub.so'
        fixture.run(fixture.TOOLS / 'clang', '--target=aarch64-linux-android24',
                    f'--sysroot={fixture.SYSROOT}', '-shared', '-nostdlib', stub, '-o', library)
        fixture.run(fixture.TOOLS / 'llvm-strip', '--strip-unneeded', library)
        original = work / 'original.apk'
        with zipfile.ZipFile(original, 'w') as apk:
            apk.writestr('AndroidManifest.xml', b'fixture only')
            apk.write(library, 'lib/arm64-v8a/libapp.so')
            apk.write(library, 'lib/arm64-v8a/libflutter.so')
            apk.writestr('META-INF/CERT.SF', b'obsolete signature')
            apk.writestr('assets/flutter_assets/assets/hello.txt', b'hello from final APK')
            apk.writestr('assets/flutter_assets/FontManifest.json', b'[]')
        for backend in ('A', 'B'):
            output = work / f'backend-{backend}.apk'
            command = [sys.executable, str(ROOT / 'scripts/package_apk.py'),
                       '--input', str(original), '--output', str(output),
                       '--backend', backend, '--assembly', str(assembly),
                       '--private', str(private), '--public', str(public),
                       '--engine-src', str(fixture.ENGINE_SRC), '--dart', str(dart)]
            checked(command)
            first = output.read_bytes()
            checked(command)
            if output.read_bytes() != first:
                raise RuntimeError(f'{backend}: unsigned packaging is not deterministic')
            with zipfile.ZipFile(output) as apk:
                paths = set(apk.namelist())
                if 'META-INF/CERT.SF' in paths or any('flutter_assets' in p for p in paths):
                    raise RuntimeError('old assets or signatures survived repacking')
                if ('lib/arm64-v8a/libpayload.so' in paths) != (backend == 'A'):
                    raise RuntimeError('unexpected payload library')
            checked(['bash', str(ROOT / 'scripts/verify_release_apk.sh'), str(output),
                     '--public', str(public)], env=env)
            print(f'Backend {backend}: deterministic packaging and structural/integrity gate PASS (unsigned fixture)')
        with zipfile.ZipFile(original, 'a') as apk:
            apk.writestr('assets/flutter_assets/../../escape.txt', b'bad path')
        rejected = subprocess.run(command, capture_output=True, text=True)
        if rejected.returncode == 0 or 'unsafe ZIP path' not in rejected.stderr:
            raise RuntimeError('traversal entry was not rejected')
        if output.read_bytes() != first:
            raise RuntimeError('invalid APK replaced previous output')
        print('Unsafe input ZIP rejected; previous output preserved PASS')


if __name__ == '__main__':
    main()
