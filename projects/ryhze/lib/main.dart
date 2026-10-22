import 'package:flutter/material.dart';

import 'app.dart';
import 'app/app_services.dart';
import 'platform/apple/first_frame_probe.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  // Two bundled images today. Bound memory before any future remote catalog grows.
  PaintingBinding.instance.imageCache
    ..maximumSize = 64
    ..maximumSizeBytes = 32 * 1024 * 1024;
  final probe = await FirstFrameProbe.connect();
  probe?.installErrorHandlers();
  try {
    final services = await AppServices.bootstrap();
    await probe?.bootstrapCompleted(
      healthy:
          services.storageFailure == null &&
          services.lifecycleFailure == null &&
          services.storage.durable,
    );
    final app = RyhzeApp(services: services);
    runApp(probe?.wrap(app) ?? app);
  } catch (_) {
    await probe?.fail('bootstrap-failed');
    rethrow;
  }
}
