#!/usr/bin/env python3
"""Link actual Android ARM64 AOT assembly and a packed payload into libapp.so.

Uses the pinned macOS-host LLVM/NDK toolchain. Reads the engine's actual snapshot
symbol contract instead of assuming the four-symbol layout from older Dart.
This checks ELF layout; successful device execution is a separate test.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def inspect(readelf, library):
    return json.loads(run(readelf, '--elf-output-style=JSON', '--sections',
                          '--program-headers', '--dyn-syms', library))[0]


def validate(info, library, payload, expected):
    require(info['FileSummary']['Format'] == 'elf64-littleaarch64', 'wrong ELF target')
    sections = {s['Section']['Name']['Name']: s['Section'] for s in info['Sections']}
    require('.flutter_payload' in sections, 'payload section missing')
    section = sections['.flutter_payload']
    require(section['Flags']['Value'] == 2, 'payload must be allocatable and read-only')
    require(not any(name.startswith('.debug_') or name == '.symtab' for name in sections),
            'debug sections or static symbols remain')
    with library.open('rb') as file:
        file.seek(section['Offset'])
        embedded = file.read(section['Size'])
    require(embedded == payload.read_bytes(), 'embedded payload differs from input')
    loads = [p['ProgramHeader'] for p in info['ProgramHeaders']
             if p['ProgramHeader']['Type']['Name'] == 'PT_LOAD']
    for segment in loads:
        require(segment['Alignment'] >= 16384, 'PT_LOAD alignment smaller than 16 KiB')
        require(segment['Flags']['Value'] & 3 != 3, 'writable executable segment')
    require(any(p['Flags']['Value'] & 6 == 4 and
                p['Offset'] <= section['Offset'] and
                section['Offset'] + section['Size'] <= p['Offset'] + p['FileSize'] and
                section['Address'] - p['VirtualAddress'] == section['Offset'] - p['Offset']
                for p in loads), 'payload is not fully backed by a read-only PT_LOAD')
    symbols = {s['Symbol']['Name']['Name']: s['Symbol'] for s in info['DynamicSymbols']
               if s['Symbol']['Name']['Name']}
    require(set(symbols) == set(expected), 'unexpected exported or undefined symbols')
    require(all(s['Section']['Value'] != 0 for s in symbols.values()), 'undefined symbol')
    start = symbols['__flutter_payload_start']['Value']
    end = symbols['__flutter_payload_end']['Value']
    require(start == section['Address'] and end - start == section['Size'],
            'payload symbols do not bracket the section')
    for name in set(expected) - {'__flutter_payload_start', '__flutter_payload_end'}:
        value = symbols[name]['Value']
        require(any(p['VirtualAddress'] <= value < p['VirtualAddress'] + p['FileSize']
                    for p in loads), f'{name} is outside the mapped image')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembly', required=True, type=Path)
    parser.add_argument('--payload', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--engine-src', required=True, type=Path)
    args = parser.parse_args()
    engine = args.engine_src.resolve() / 'flutter'
    tools = engine / 'buildtools/mac-x64/clang/bin'
    sysroot = engine / ('third_party/android_tools/sdk/ndk/28.2.13676358/'
                        'toolchains/llvm/prebuilt/darwin-x86_64/sysroot')
    source = (engine / 'runtime/dart_snapshot.cc').read_text()
    snapshot_symbols = sorted(set(re.findall(
        r'const char\* DartSnapshot::k(?:VM|Isolate)(?:Data|Instructions)Symbol\s*=\s*"([A-Za-z0-9_]+)";',
        source)))
    require(len(snapshot_symbols) in (2, 4), 'unrecognized engine snapshot symbol contract')
    expected = snapshot_symbols + ['__flutter_payload_start', '__flutter_payload_end']
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    target = ['--target=aarch64-linux-android24', f'--sysroot={sysroot}']
    with tempfile.TemporaryDirectory(prefix='packed-backend-b-', dir=args.output.parent) as directory:
        work = Path(directory)
        app, payload = work / 'app.o', work / 'payload.o'
        run(tools / 'clang', *target, '-c', args.assembly.resolve(), '-o', app)
        run(tools / 'clang', *target, f'-DPAYLOAD_FILE="{args.payload.resolve()}"',
            '-c', HERE / 'payload_section.S', '-o', payload)
        available = {line.split()[-1] for line in run(
            tools / 'llvm-nm', '--defined-only', '--extern-only', app).splitlines() if line}
        aliases = {}
        for name in snapshot_symbols:
            if name not in available:
                require('_' + name in available, f'AOT assembly does not define {name}')
                aliases[name] = '_' + name
        version_script = work / 'libapp.ver'
        version_script.write_text('{ global: ' + '; '.join(expected) + '; local: *; };\n')
        library = work / 'libapp.so'
        run(tools / 'clang', *target, '-shared', '-nostdlib',
            '-Wl,-soname,libapp.so', '-Wl,--no-undefined', '-Wl,--gc-sections',
            '-Wl,-z,max-page-size=16384', '-Wl,-z,noexecstack', '-Wl,--build-id=sha1',
            f'-Wl,--version-script,{version_script}', f'-Wl,-T,{HERE / "payload_keep.ld"}',
            *[f'-Wl,--defsym,{name}={original}' for name, original in aliases.items()],
            app, payload, '-o', library)
        run(tools / 'llvm-strip', '--strip-unneeded', library)
        info = inspect(tools / 'llvm-readelf', library)
        validate(info, library, args.payload, expected)
        section = next(s['Section'] for s in info['Sections']
                       if s['Section']['Name']['Name'] == '.flutter_payload')
        payload_segment = next(p['ProgramHeader'] for p in info['ProgramHeaders']
                               if p['ProgramHeader']['Type']['Name'] == 'PT_LOAD' and
                               p['ProgramHeader']['Offset'] <= section['Offset'] <
                               p['ProgramHeader']['Offset'] + p['ProgramHeader']['FileSize'])
        report = {'backend': 'B', 'library_bytes': library.stat().st_size,
                  'sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                  'payload_bytes': args.payload.stat().st_size, 'exports': expected,
                  'snapshot_aliases': aliases,
                  'payload_segment_executable': bool(payload_segment['Flags']['Value'] & 1),
                  'device_tested': False}
        library.replace(args.output)
        args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
