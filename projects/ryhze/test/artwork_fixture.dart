import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:ryhze/design/artwork_reference.dart';

ArtworkReference artworkRef([int id = 0, int revision = 1]) => ArtworkReference(
  itemId: 'test-$id',
  revision: revision,
  path: '/art/test-$id.png',
  note: 'World concept artwork · Not gameplay',
);

Uint8List artworkPng({
  int width = 1,
  int height = 1,
  bool corruptPixels = false,
  bool animated = false,
  int padding = 0,
}) {
  final builder = BytesBuilder();
  builder.add([137, 80, 78, 71, 13, 10, 26, 10]);
  void chunk(String type, List<int> payload) {
    final bytes = Uint8List(12 + payload.length);
    final data = ByteData.sublistView(bytes);
    data.setUint32(0, payload.length);
    bytes.setRange(4, 8, ascii.encode(type));
    bytes.setRange(8, 8 + payload.length, payload);
    var crc = 0xffffffff;
    for (var i = 4; i < 8 + payload.length; ++i) {
      crc ^= bytes[i];
      for (var bit = 0; bit < 8; ++bit) {
        crc = (crc >>> 1) ^ ((crc & 1) == 0 ? 0 : 0xedb88320);
      }
    }
    data.setUint32(bytes.length - 4, crc ^ 0xffffffff);
    builder.add(bytes);
  }

  final header = Uint8List(13);
  ByteData.sublistView(header)
    ..setUint32(0, width)
    ..setUint32(4, height);
  header[8] = 8;
  header[9] = 6;
  chunk('IHDR', header);
  if (animated) chunk('acTL', List.filled(8, 0));
  if (padding > 0) chunk('tEXt', List.filled(padding, 65));
  chunk(
    'IDAT',
    corruptPixels ? [1, 2, 3] : zlib.encode([0, 100, 150, 200, 255]),
  );
  chunk('IEND', []);
  return builder.takeBytes();
}
