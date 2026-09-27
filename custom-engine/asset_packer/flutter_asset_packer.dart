import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'packer_format.dart';

class _Args {
  String? input;
  String? output;
  String? inspect;
  String? against;
  String hash = 'fnv1a';
  String compression = 'none';
  int alignment = 16;
}

_Args _parse(List<String> argv) {
  final a = _Args();
  for (int i = 0; i < argv.length; i++) {
    final arg = argv[i];
    String next() {
      if (i + 1 >= argv.length) {
        _fail('missing value for $arg');
      }
      return argv[++i];
    }

    switch (arg) {
      case '--input':
        a.input = next();
      case '--output':
        a.output = next();
      case '--inspect':
        a.inspect = next();
      case '--against':
        a.against = next();
      case '--hash':
        a.hash = next();
      case '--compression':
        a.compression = next();
      case '--alignment':
        a.alignment = int.parse(next());
      case '-h':
      case '--help':
        _usage();
        exit(0);
      default:
        _fail('unknown argument: $arg');
    }
  }
  return a;
}

Never _fail(String message) {
  stderr.writeln('flutter_asset_packer: $message');
  exit(2);
}

void _usage() {
  stdout.writeln('flutter_asset_packer\n'
      '  --input <dir> --output <dir> [--hash fnv1a]'
      ' [--compression auto|none|zlib] [--alignment N]\n'
      '  --inspect <payload.bin> [--against <dir>]');
}

const int _kMinSaveBytes = 64;
const double _kMinSaveRatio = 0.05;

class _Entry {
  _Entry(this.keyHash, this.key, this.origSize, this.stored, this.codec);
  final int keyHash;
  final String key;
  final int origSize;
  final Uint8List stored;
  final int codec;
  late int offset;
}

String _extOf(String key) {
  final slash = key.lastIndexOf('/');
  final name = slash < 0 ? key : key.substring(slash + 1);
  final dot = name.lastIndexOf('.');
  return dot < 0 ? '' : name.substring(dot + 1).toLowerCase();
}

Uint8List _chooseStored(String mode, String key, Uint8List data,
    {required List<int> codecOut}) {
  if (mode == 'none' || data.isEmpty) {
    codecOut[0] = kCodecNone;
    return data;
  }
  if (mode == 'auto' && kAlreadyCompressedExt.contains(_extOf(key))) {
    codecOut[0] = kCodecNone;
    return data;
  }
  final compressed = Uint8List.fromList(zlib.encode(data));
  if (mode == 'zlib') {
    if (compressed.length < data.length) {
      codecOut[0] = kCodecZlib;
      return compressed;
    }
    codecOut[0] = kCodecNone;
    return data;
  }
  final saved = data.length - compressed.length;
  if (saved >= _kMinSaveBytes && saved >= (data.length * _kMinSaveRatio)) {
    codecOut[0] = kCodecZlib;
    return compressed;
  }
  codecOut[0] = kCodecNone;
  return data;
}

void main(List<String> argv) {
  if (argv.isEmpty) {
    _usage();
    exit(2);
  }
  final args = _parse(argv);
  if (args.inspect != null) {
    exit(_inspect(args));
  }
  exit(_pack(args));
}

int _pack(_Args args) {
  if (args.input == null || args.output == null) {
    _fail('pack mode requires --input and --output');
  }
  if (args.hash != 'fnv1a') {
    _fail("hash '${args.hash}' not supported (use fnv1a)");
  }
  if (args.compression != 'auto' &&
      args.compression != 'none' &&
      args.compression != 'zlib') {
    _fail("compression '${args.compression}' not available in-tree "
        '(use auto|none|zlib; see docs/backend-b-elf.md for zstd/lz4)');
  }
  if (args.alignment < 1 || (args.alignment & (args.alignment - 1)) != 0) {
    _fail('alignment must be a power of two, got ${args.alignment}');
  }

  final inputDir = Directory(args.input!);
  if (!inputDir.existsSync()) {
    _fail('input dir not found: ${args.input}');
  }
  final rootPath = inputDir.absolute.path;

  final entries = <_Entry>[];
  final seen = <int, String>{};
  for (final e in inputDir.listSync(recursive: true, followLinks: false)) {
    if (e is! File) {
      continue;
    }
    var key = e.absolute.path.substring(rootPath.length + 1);
    key = key.replaceAll(Platform.pathSeparator, '/');
    final hash = fnv1a64(key);
    final prior = seen[hash];
    if (prior != null) {
      _fail('fnv1a64 collision between "$prior" and "$key"');
    }
    seen[hash] = key;
    final data = e.readAsBytesSync();
    final codecOut = <int>[kCodecNone];
    final stored =
        _chooseStored(args.compression, key, data, codecOut: codecOut);
    entries.add(_Entry(hash, key, data.length, stored, codecOut[0]));
  }

  entries.sort((x, y) => compareUint64(x.keyHash, y.keyHash));

  final count = entries.length;
  final indexOffset = kHeaderSize;
  final indexEnd = indexOffset + count * kEntrySize;
  final blobOffset = alignUp(indexEnd, args.alignment);

  int cursor = 0;
  for (final en in entries) {
    cursor = alignUp(cursor, args.alignment);
    en.offset = cursor;
    cursor += en.stored.length;
  }
  final blobSize = cursor;

  final out = Uint8List(blobOffset + blobSize);
  final bd = ByteData.sublistView(out);
  for (int i = 0; i < kMagic.length; i++) {
    out[i] = kMagic[i];
  }
  bd.setUint16(4, kVersion, Endian.little);
  bd.setUint16(6, kHeaderFlagEntryCrc32, Endian.little);
  bd.setUint32(8, count, Endian.little);
  bd.setUint32(12, indexOffset, Endian.little);
  bd.setUint64(16, blobOffset, Endian.little);
  bd.setUint64(24, 0, Endian.little);

  for (int i = 0; i < count; i++) {
    final en = entries[i];
    final row = indexOffset + i * kEntrySize;
    bd.setUint64(row, en.keyHash, Endian.little);
    bd.setUint64(row + 8, en.offset, Endian.little);
    bd.setUint32(row + 16, en.stored.length, Endian.little);
    bd.setUint32(row + 20, en.origSize, Endian.little);
    bd.setUint32(row + 24, en.codec, Endian.little);
    bd.setUint32(row + 28, crc32(en.stored), Endian.little);
    out.setRange(blobOffset + en.offset,
        blobOffset + en.offset + en.stored.length, en.stored);
  }

  final outputDir = Directory(args.output!);
  outputDir.createSync(recursive: true);
  final payloadFile = File('${outputDir.path}/payload.bin');
  payloadFile.writeAsBytesSync(out);

  final meta = <String, Object>{
    'format': 'FEAP',
    'version': kVersion,
    'hash': args.hash,
    'compression': args.compression,
    'alignment': args.alignment,
    'count': count,
    'indexOffset': indexOffset,
    'blobOffset': blobOffset,
    'payloadSize': out.length,
    'payloadCrc32': crc32(out),
    'entries': [
      for (final en in entries)
        {
          'keyHash': hex64(en.keyHash),
          'offset': en.offset,
          'storedSize': en.stored.length,
          'originalSize': en.origSize,
          'codec': codecName(en.codec),
          'crc32': crc32(en.stored),
        },
    ],
  };
  final metaFile = File('${outputDir.path}/payload_meta.json');
  metaFile.writeAsStringSync(
      const JsonEncoder.withIndent('  ').convert(meta) + '\n');

  final compressedCount = entries.where((e) => e.codec != kCodecNone).length;
  final origTotal = entries.fold<int>(0, (s, e) => s + e.origSize);
  final storedTotal = entries.fold<int>(0, (s, e) => s + e.stored.length);
  stdout.writeln('packed $count assets ($compressedCount compressed)');
  stdout.writeln('  payload      : ${payloadFile.path} (${out.length} bytes)');
  stdout.writeln('  metadata     : ${metaFile.path}');
  stdout.writeln('  blob offset  : $blobOffset (alignment ${args.alignment})');
  stdout.writeln('  original blob: $origTotal -> stored blob: $storedTotal');
  stdout.writeln('  payload crc32: ${crc32(out).toRadixString(16)}');
  return 0;
}

int _inspect(_Args args) {
  final bytes = File(args.inspect!).readAsBytesSync();
  if (bytes.length < kHeaderSize) {
    _fail('file smaller than header');
  }
  for (int i = 0; i < kMagic.length; i++) {
    if (bytes[i] != kMagic[i]) {
      _fail('bad magic');
    }
  }
  final bd = ByteData.sublistView(bytes);
  final version = bd.getUint16(4, Endian.little);
  final headerFlags = bd.getUint16(6, Endian.little);
  final count = bd.getUint32(8, Endian.little);
  final indexOffset = bd.getUint32(12, Endian.little);
  final blobOffset = bd.getUint64(16, Endian.little);
  final hasCrc = (headerFlags & kHeaderFlagEntryCrc32) != 0;

  final names = <int, String>{};
  if (args.against != null) {
    final dir = Directory(args.against!);
    final rootPath = dir.absolute.path;
    for (final e in dir.listSync(recursive: true, followLinks: false)) {
      if (e is! File) {
        continue;
      }
      var key = e.absolute.path.substring(rootPath.length + 1);
      key = key.replaceAll(Platform.pathSeparator, '/');
      names[fnv1a64(key)] = key;
    }
  }

  stdout.writeln('FEAP payload: ${args.inspect}');
  stdout.writeln('  version      : $version');
  stdout.writeln('  count        : $count');
  stdout.writeln('  index offset : $indexOffset');
  stdout.writeln('  blob offset  : $blobOffset');
  stdout.writeln('  file size    : ${bytes.length}');
  stdout.writeln('  payload crc32: ${crc32(bytes).toRadixString(16)}');
  stdout.writeln('  crc per-entry: ${hasCrc ? "present" : "absent"}');
  stdout.writeln('  idx  key_hash            offset      stored     orig  codec '
      'crc   name');
  int badCrc = 0;
  for (int i = 0; i < count; i++) {
    final row = indexOffset + i * kEntrySize;
    final keyHash = bd.getUint64(row, Endian.little);
    final offset = bd.getUint64(row + 8, Endian.little);
    final stored = bd.getUint32(row + 16, Endian.little);
    final orig = bd.getUint32(row + 20, Endian.little);
    final codec = bd.getUint32(row + 24, Endian.little) & kCodecMask;
    final checksum = bd.getUint32(row + 28, Endian.little);
    final start = blobOffset + offset;
    var crcStatus = '-';
    if (hasCrc) {
      final actual = crc32(bytes, start, start + stored);
      final ok = actual == checksum;
      if (!ok) {
        badCrc++;
      }
      crcStatus = ok ? 'ok' : 'BAD';
    }
    final name = names[keyHash] ?? '';
    stdout.writeln('  ${i.toString().padLeft(3)}  ${hex64(keyHash)}  '
        '${offset.toString().padLeft(10)}  ${stored.toString().padLeft(9)}  '
        '${orig.toString().padLeft(7)}  ${codecName(codec).padRight(5)} '
        '${crcStatus.padRight(3)}   $name');
  }
  if (hasCrc && badCrc > 0) {
    stderr.writeln('integrity: $badCrc entr(y|ies) FAILED crc32');
    return 1;
  }
  return 0;
}
