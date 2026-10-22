import 'dart:typed_data';

import '../core/app_failure.dart';

const publicArtworkMaximumBytes = 8 * 1024 * 1024;

class PublicArtworkPngInfo {
  const PublicArtworkPngInfo(this.width, this.height);
  final int width, height;
}

final _crcTable = Uint32List.fromList(
  List.generate(256, (value) {
    var crc = value;
    for (var i = 0; i < 8; ++i) {
      crc = (crc >>> 1) ^ ((crc & 1) == 0 ? 0 : 0xedb88320);
    }
    return crc;
  }),
);

/// Validate bounded, static PNG structure before passing compressed bytes to
/// Flutter's decoder. A valid structure/CRC does not prove valid pixel data.
PublicArtworkPngInfo validatePublicArtworkPng(Uint8List bytes) {
  Never invalid() => throw const AppFailure(FailureKind.invalidData);
  if (bytes.length < 57 || bytes.length > publicArtworkMaximumBytes) invalid();
  const signature = [137, 80, 78, 71, 13, 10, 26, 10];
  for (var i = 0; i < 8; i++) {
    if (bytes[i] != signature[i]) invalid();
  }
  final data = ByteData.sublistView(bytes);
  var offset = 8, chunks = 0, width = 0, height = 0, color = 0, depth = 0;
  var idat = false, endedIdat = false, palette = false;
  while (offset < bytes.length) {
    if (++chunks > 4096 || bytes.length - offset < 12) invalid();
    final length = data.getUint32(offset);
    if (length > bytes.length - offset - 12) invalid();
    final type = String.fromCharCodes(bytes.sublist(offset + 4, offset + 8));
    if (!RegExp(r'^[A-Za-z]{4}$').hasMatch(type) ||
        bytes[offset + 6] & 32 != 0) {
      invalid();
    }
    var crc = 0xffffffff;
    for (var i = offset + 4; i < offset + 8 + length; ++i) {
      crc = _crcTable[(crc ^ bytes[i]) & 255] ^ (crc >>> 8);
    }
    if ((crc ^ 0xffffffff) != data.getUint32(offset + 8 + length)) invalid();
    if (type == 'acTL' || type == 'fcTL' || type == 'fdAT') invalid();
    // Public artwork does not need compressed metadata/profiles. Refuse those
    // independent decompression inputs before invoking the image decoder.
    if (type == 'iCCP' || type == 'zTXt' || type == 'iTXt') invalid();
    if (chunks == 1) {
      if (type != 'IHDR' || length != 13) invalid();
      width = data.getUint32(offset + 8);
      height = data.getUint32(offset + 12);
      depth = bytes[offset + 16];
      color = bytes[offset + 17];
      final depths = switch (color) {
        0 => const [1, 2, 4, 8, 16],
        2 => const [8, 16],
        3 => const [1, 2, 4, 8],
        4 || 6 => const [8, 16],
        _ => const <int>[],
      };
      if (width < 1 ||
          height < 1 ||
          width > 8192 ||
          height > 8192 ||
          width * height > 16 * 1024 * 1024 ||
          !depths.contains(depth) ||
          bytes[offset + 18] != 0 ||
          bytes[offset + 19] != 0 ||
          bytes[offset + 20] > 1) {
        invalid();
      }
    } else if (type == 'IHDR') {
      invalid();
    } else if (type == 'IDAT') {
      if (endedIdat || (color == 3 && !palette)) invalid();
      idat = true;
    } else {
      if (idat) endedIdat = true;
      if (type == 'PLTE') {
        if (palette ||
            idat ||
            color == 0 ||
            color == 4 ||
            length < 3 ||
            length > 768 ||
            length % 3 != 0 ||
            (color == 3 && length ~/ 3 > 1 << depth)) {
          invalid();
        }
        palette = true;
      } else if (type == 'IEND') {
        if (!idat || length != 0 || offset + 12 != bytes.length) invalid();
        return PublicArtworkPngInfo(width, height);
      } else if (bytes[offset + 4] & 32 == 0) {
        invalid();
      }
    }
    offset += length + 12;
  }
  invalid();
}
