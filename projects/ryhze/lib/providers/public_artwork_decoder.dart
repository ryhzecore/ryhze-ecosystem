import 'dart:typed_data';
import 'dart:ui' as ui;

import '../core/app_failure.dart';

Future<void> validateDecodedPublicArtwork(Uint8List bytes) async {
  // The repository has already checked every chunk/CRC off the UI isolate.
  final header = ByteData.sublistView(bytes);
  final width = header.getUint32(16), height = header.getUint32(20);
  ui.Codec? codec;
  ui.Image? image;
  try {
    // Validate pixels without retaining a full-size decoded image. Static input
    // is already bounded to 16 megapixels and the validation output to 2048px.
    final scale = (2048 / (width > height ? width : height)).clamp(0.0, 1.0);
    codec = await ui.instantiateImageCodec(
      bytes,
      targetWidth: (width * scale).round().clamp(1, 2048),
      targetHeight: (height * scale).round().clamp(1, 2048),
    );
    if (codec.frameCount != 1) throw const AppFailure(FailureKind.invalidData);
    image = (await codec.getNextFrame()).image;
    if (image.width < 1 || image.height < 1) {
      throw const AppFailure(FailureKind.invalidData);
    }
  } catch (_) {
    throw const AppFailure(FailureKind.invalidData);
  } finally {
    image?.dispose();
    codec?.dispose();
  }
}
