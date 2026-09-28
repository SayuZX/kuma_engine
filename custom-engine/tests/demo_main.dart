import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() => runApp(const PackedAssetDemo());

class PackedAssetDemo extends StatefulWidget {
  const PackedAssetDemo({super.key});

  @override
  State<PackedAssetDemo> createState() => _PackedAssetDemoState();
}

class _PackedAssetDemoState extends State<PackedAssetDemo> {
  String status = 'Loading packed assets';

  @override
  void initState() {
    super.initState();
    _loadAssets();
  }

  Future<void> _loadAssets() async {
    try {
      String decode(ByteData data) => utf8.decode(data.buffer.asUint8List(
            data.offsetInBytes,
            data.lengthInBytes,
          ));
      final values = await Future.wait([
        rootBundle.load('assets/message.txt'),
        rootBundle.load('assets/data.json'),
      ]);
      final message = decode(values[0]).trim();
      final data = jsonDecode(decode(values[1])) as Map<String, dynamic>;
      if (mounted) {
        setState(() => status = '$message (${data['version']})');
      }
    } catch (error) {
      if (mounted) {
        setState(() => status = 'Asset loading failed: $error');
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('Packed asset demo')),
        body: Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Image.asset('assets/logo.png', width: 96, height: 96),
              const SizedBox(height: 20),
              Text(status),
            ],
          ),
        ),
      ),
    );
  }
}
