import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

int _zlibSize(List<int> data) => zlib.encode(data).length;

String _pad(Object v, int w) => v.toString().padLeft(w);

void main() {
  final samples = <String, Uint8List>{};

  final repetitiveJson = StringBuffer();
  for (var i = 0; i < 800; i++) {
    repetitiveJson.write('{"row":123456,"name":"item"}\n');
  }
  samples['repetitive.json'] =
      Uint8List.fromList(repetitiveJson.toString().codeUnits);

  const english = 'the quick brown fox jumps over the lazy dog. ';
  final prose = StringBuffer();
  for (var i = 0; i < 300; i++) {
    prose.write(english);
  }
  samples['prose.txt'] = Uint8List.fromList(prose.toString().codeUnits);

  samples['config.json'] =
      Uint8List.fromList('{"debug":false,"level":3,"name":"app"}'.codeUnits);

  samples['tiny.txt'] = Uint8List.fromList('ok\n'.codeUnits);

  final rng = Random(42);
  samples['random.bin'] =
      Uint8List.fromList(List<int>.generate(8192, (_) => rng.nextInt(256)));

  final svg = StringBuffer('<svg xmlns="http://www.w3.org/2000/svg">');
  for (var i = 0; i < 200; i++) {
    svg.write('<rect x="$i" y="$i" width="10" height="10" fill="#abcdef"/>');
  }
  svg.write('</svg>');
  samples['icon.svg'] = Uint8List.fromList(svg.toString().codeUnits);

  const minSaveBytes = 64;
  const minSaveRatio = 0.05;

  stdout.writeln('sample            orig   zlib   saved   ratio   auto-decision');
  for (final e in samples.entries) {
    final orig = e.value.length;
    final z = _zlibSize(e.value);
    final saved = orig - z;
    final ratio = orig == 0 ? 0.0 : z / orig;
    final compress = saved >= minSaveBytes && saved >= orig * minSaveRatio;
    stdout.writeln('${e.key.padRight(16)} ${_pad(orig, 6)} ${_pad(z, 6)} '
        '${_pad(saved, 6)}  ${(ratio * 100).toStringAsFixed(1).padLeft(5)}%  '
        '${compress ? "zlib" : "none"}');
  }

  stdout.writeln('');
  stdout.writeln('threshold: store zlib only when saved >= $minSaveBytes bytes '
      'AND >= ${(minSaveRatio * 100).toStringAsFixed(0)}% of original.');
  stdout.writeln('rationale: zlib adds ~11 bytes of framing, so tiny or '
      'incompressible inputs lose or barely gain; the floor avoids paying '
      'decompression cost for negligible or negative size wins. Already-'
      'compressed extensions (png/jpg/webp/mp3/mp4/...) are skipped before '
      'this test.');
}
