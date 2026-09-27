import 'dart:convert';

const List<int> kMagic = [0x46, 0x45, 0x41, 0x50];
const int kVersion = 2;
const int kHeaderSize = 32;
const int kEntrySize = 32;
const int kCodecMask = 0xff;
const int kCodecNone = 0;
const int kCodecZlib = 1;

const int kHeaderFlagEntryCrc32 = 1;

const Set<String> kAlreadyCompressedExt = {
  'png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp',
  'mp3', 'aac', 'm4a', 'ogg', 'opus',
  'mp4', 'm4v', 'mov', 'webm',
  'woff', 'woff2', 'zip', 'gz', 'jar',
};

String codecName(int codec) {
  switch (codec) {
    case kCodecNone:
      return 'none';
    case kCodecZlib:
      return 'zlib';
    default:
      return 'unknown($codec)';
  }
}

const int _kMinInt64 = -0x8000000000000000;

int fnv1a64(String s) {
  const int prime = 0x100000001b3;
  int hash = -3750763034362895579;
  for (final b in utf8.encode(s)) {
    hash ^= b;
    hash = hash * prime;
  }
  return hash;
}

int compareUint64(int a, int b) => (a ^ _kMinInt64).compareTo(b ^ _kMinInt64);

String hex64(int v) {
  final bytes = <String>[];
  for (int i = 7; i >= 0; i--) {
    final byte = (v >> (8 * i)) & 0xff;
    bytes.add(byte.toRadixString(16).padLeft(2, '0'));
  }
  return '0x${bytes.join()}';
}

int alignUp(int value, int alignment) {
  if (alignment <= 1) {
    return value;
  }
  final remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

final List<int> _crcTable = _buildCrcTable();

List<int> _buildCrcTable() {
  final table = List<int>.filled(256, 0);
  for (int n = 0; n < 256; n++) {
    int c = n;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) != 0 ? (0xedb88320 ^ (c >> 1)) : (c >> 1);
    }
    table[n] = c;
  }
  return table;
}

int crc32(List<int> data, [int start = 0, int? end]) {
  int crc = 0xffffffff;
  final limit = end ?? data.length;
  for (int i = start; i < limit; i++) {
    crc = _crcTable[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  }
  return (crc ^ 0xffffffff) & 0xffffffff;
}
