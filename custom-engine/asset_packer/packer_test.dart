import 'dart:io';
import 'dart:typed_data';

import 'packer_format.dart';

int _failures = 0;

void _check(bool ok, String label) {
  if (ok) {
    stdout.writeln('  ok   $label');
  } else {
    stdout.writeln('  FAIL $label');
    _failures++;
  }
}

Uint8List? _read(Uint8List bytes, String key) {
  final bd = ByteData.sublistView(bytes);
  final count = bd.getUint32(8, Endian.little);
  final indexOffset = bd.getUint32(12, Endian.little);
  final blobOffset = bd.getUint64(16, Endian.little);
  final target = fnv1a64(key);
  var lo = 0;
  var hi = count - 1;
  while (lo <= hi) {
    final mid = (lo + hi) >> 1;
    final row = indexOffset + mid * kEntrySize;
    final h = bd.getUint64(row, Endian.little);
    final c = compareUint64(h, target);
    if (c == 0) {
      final offset = bd.getUint64(row + 8, Endian.little);
      final stored = bd.getUint32(row + 16, Endian.little);
      final codec = bd.getUint32(row + 24, Endian.little) & kCodecMask;
      final start = blobOffset + offset;
      final block = Uint8List.sublistView(bytes, start, start + stored);
      if (codec == kCodecZlib) {
        return Uint8List.fromList(zlib.decode(block));
      }
      return block;
    } else if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return null;
}

int _codecOf(Uint8List bytes, String key) {
  final bd = ByteData.sublistView(bytes);
  final count = bd.getUint32(8, Endian.little);
  final indexOffset = bd.getUint32(12, Endian.little);
  final target = fnv1a64(key);
  for (var i = 0; i < count; i++) {
    final row = indexOffset + i * kEntrySize;
    if (bd.getUint64(row, Endian.little) == target) {
      return bd.getUint32(row + 24, Endian.little) & kCodecMask;
    }
  }
  return -1;
}

bool _eq(List<int> a, List<int> b) {
  if (a.length != b.length) {
    return false;
  }
  for (var i = 0; i < a.length; i++) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

void _runPacker(String input, String output, int alignment,
    {String compression = 'auto'}) {
  final script = '${Directory(Platform.script.toFilePath()).parent.path}'
      '/flutter_asset_packer.dart';
  final result = Process.runSync(Platform.resolvedExecutable, [
    script,
    '--input',
    input,
    '--output',
    output,
    '--alignment',
    '$alignment',
    '--compression',
    compression,
  ]);
  if (result.exitCode != 0) {
    stdout.writeln(result.stdout);
    stderr.writeln(result.stderr);
    throw StateError('packer failed');
  }
}

void main() {
  final tmp = Directory.systemTemp.createTempSync('feap_packer_test_');
  try {
    final assets = {
      'assets/hello.txt': 'Hello from packed payload!\n',
      'assets/data/config.json': '{"debug":false,"level":3}',
      'assets/images/logo.bin': String.fromCharCodes(
          List<int>.generate(777, (i) => (i * 37 + 11) & 0xff)),
      'AssetManifest.bin': 'manifest-bytes-xyz',
      'assets/repeat.txt': 'A' * 3000,
      'assets/photo.png': String.fromCharCodes(
          List<int>.generate(2000, (i) => (i * 131 + 7) & 0xff)),
    };
    final inputDir = Directory('${tmp.path}/in')..createSync();
    assets.forEach((key, value) {
      final f = File('${inputDir.path}/$key');
      f.parent.createSync(recursive: true);
      f.writeAsStringSync(value);
    });

    stdout.writeln('determinism:');
    _runPacker(inputDir.path, '${tmp.path}/out1', 16);
    _runPacker(inputDir.path, '${tmp.path}/out2', 16);
    final p1 = File('${tmp.path}/out1/payload.bin').readAsBytesSync();
    final p2 = File('${tmp.path}/out2/payload.bin').readAsBytesSync();
    _check(_eq(p1, p2), 'payload.bin byte-identical across two runs');

    stdout.writeln('round-trip:');
    final payload = Uint8List.fromList(p1);
    var allOk = true;
    assets.forEach((key, value) {
      final got = _read(payload, key);
      final want = File('${inputDir.path}/$key').readAsBytesSync();
      if (got == null || !_eq(got, want)) {
        allOk = false;
        stdout.writeln('    mismatch: $key');
      }
    });
    _check(allOk, 'every asset reads back identical');
    _check(_read(payload, 'assets/missing.txt') == null,
        'missing key returns null');

    stdout.writeln('alignment:');
    final bd = ByteData.sublistView(payload);
    final count = bd.getUint32(8, Endian.little);
    final indexOffset = bd.getUint32(12, Endian.little);
    final blobOffset = bd.getUint64(16, Endian.little);
    var aligned = blobOffset % 16 == 0;
    for (var i = 0; i < count; i++) {
      final offset = bd.getUint64(indexOffset + i * kEntrySize + 8,
          Endian.little);
      if ((blobOffset + offset) % 16 != 0) {
        aligned = false;
      }
    }
    _check(aligned, 'every block start is 16-byte aligned');

    stdout.writeln('sorted-index:');
    var sorted = true;
    for (var i = 1; i < count; i++) {
      final prev = bd.getUint64(indexOffset + (i - 1) * kEntrySize,
          Endian.little);
      final cur = bd.getUint64(indexOffset + i * kEntrySize, Endian.little);
      if (compareUint64(prev, cur) >= 0) {
        sorted = false;
      }
    }
    _check(sorted, 'index sorted ascending by unsigned key_hash');

    stdout.writeln('auto-compression:');
    _check(_codecOf(payload, 'assets/repeat.txt') == kCodecZlib,
        'highly compressible text is stored zlib');
    _check(_codecOf(payload, 'assets/photo.png') == kCodecNone,
        'already-compressed .png extension is stored uncompressed');
    _check(_codecOf(payload, 'AssetManifest.bin') == kCodecNone,
        'tiny asset below save threshold is stored uncompressed');

    stdout.writeln('compression modes:');
    _runPacker(inputDir.path, '${tmp.path}/none', 16, compression: 'none');
    final pNone = File('${tmp.path}/none/payload.bin').readAsBytesSync();
    _check(_codecOf(Uint8List.fromList(pNone), 'assets/repeat.txt') == kCodecNone,
        '--compression none never compresses');
  } finally {
    tmp.deleteSync(recursive: true);
  }

  stdout.writeln(_failures == 0 ? 'ALL PASS' : '$_failures FAILURE(S)');
  exit(_failures == 0 ? 0 : 1);
}
