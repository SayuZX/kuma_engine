#!/usr/bin/env python3
"""Pack the exact final Flutter bundle from an APK into a native library.

Produces an UNSIGNED APK. Run zipalign, apksigner and verify_release_apk.sh
afterward. This post-build stage avoids packing stale pre-tree-shaking assets.
"""

import argparse
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'assets/flutter_assets/'


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def signature_entry(name):
    upper = name.upper()
    return upper.startswith('META-INF/') and (
        upper.endswith(('.SF', '.RSA', '.DSA', '.EC')) or upper == 'META-INF/MANIFEST.MF')


def validate_names(infos):
    names = [info.filename for info in infos]
    if len(names) != len(set(names)):
        raise ValueError('duplicate ZIP entry')
    for name in names:
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or '\0' in name:
            raise ValueError(f'unsafe ZIP path: {name!r}')
        if str(path) != name.rstrip('/'):
            raise ValueError(f'noncanonical ZIP path: {name!r}')
    if not {'lib/arm64-v8a/libapp.so', 'lib/arm64-v8a/libflutter.so'} <= set(names):
        raise ValueError('APK must contain ARM64 libapp.so and libflutter.so')
    if any(name.startswith('lib/') and not name.startswith('lib/arm64-v8a/')
           and name != 'lib/' for name in names):
        raise ValueError('APK contains an ABI other than arm64-v8a')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--backend', choices=['A', 'B'], default='A')
    parser.add_argument('--assembly', type=Path, help='matching release AOT assembly for Backend B')
    parser.add_argument('--private', required=True, type=Path)
    parser.add_argument('--public', required=True, type=Path)
    parser.add_argument('--engine-src', required=True, type=Path)
    parser.add_argument('--dart', required=True, type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error('input and output must differ')
    if args.backend == 'B' and args.assembly is None:
        parser.error('Backend B requires --assembly from the matching release build')
    signer = args.engine_src / 'out/host_debug_unopt/asset_signer'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='packed-apk-', dir=args.output.parent) as directory:
        work = Path(directory)
        assets = work / 'flutter_assets'
        with zipfile.ZipFile(args.input) as original:
            validate_names(original.infolist())
            asset_infos = [entry for entry in original.infolist()
                           if entry.filename.startswith(PREFIX) and not entry.is_dir()]
            if not asset_infos:
                raise ValueError('input APK has no Flutter bundle to pack')
            for entry in asset_infos:
                target = assets / entry.filename[len(PREFIX):]
                target.parent.mkdir(parents=True, exist_ok=True)
                # Exclusive creation also rejects case/Unicode aliases on a
                # case-insensitive host filesystem rather than losing an asset.
                with target.open('xb') as destination:
                    destination.write(original.read(entry))
            packed = work / 'packed'
            run(args.dart, ROOT / 'asset_packer/flutter_asset_packer.dart',
                '--input', assets, '--output', packed, '--compression', 'auto',
                '--hash', 'fnv1a', '--alignment', '16')
            signed = work / 'payload.signed.bin'
            run(signer, 'sign', '--input', packed / 'payload.bin', '--output', signed,
                '--private', args.private)
            run(signer, 'verify', '--input', signed, '--public', args.public)
            library = work / ('libpayload.so' if args.backend == 'A' else 'libapp.so')
            if args.backend == 'A':
                run('bash', ROOT / 'linker/build_libpayload.sh', signed, library,
                    env=dict(os.environ, ENGINE_SRC=str(args.engine_src.resolve())))
            else:
                run(sys.executable, ROOT / 'linker/build_backend_b.py',
                    '--engine-src', args.engine_src, '--assembly', args.assembly,
                    '--payload', signed, '--output', library)
            native_name = 'lib/arm64-v8a/' + library.name
            unsigned = work / 'unsigned.apk'
            with zipfile.ZipFile(unsigned, 'w') as output:
                for entry in original.infolist():
                    name = entry.filename
                    if (name.startswith(PREFIX) or name == PREFIX.rstrip('/') or
                            name == 'lib/arm64-v8a/libpayload.so' or name == native_name or
                            signature_entry(name)):
                        continue
                    output.writestr(entry, original.read(entry))
                info = zipfile.ZipInfo(native_name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED
                info.external_attr = 0o100644 << 16
                output.writestr(info, library.read_bytes())
            with zipfile.ZipFile(unsigned) as check:
                if any(name.startswith(PREFIX) for name in check.namelist()):
                    raise RuntimeError('Flutter assets remain in candidate APK')
            unsigned.replace(args.output)
            print(f'UNSIGNED Backend {args.backend} candidate: {args.output}; {len(asset_infos)} assets packed')
            print('Next: zipalign, apksigner, then verify_release_apk.sh. Device test still required.')


if __name__ == '__main__':
    main()
