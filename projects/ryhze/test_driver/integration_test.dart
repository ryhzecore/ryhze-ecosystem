import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_driver/flutter_driver.dart';

import 'journey_result.dart';

Future<void> main() async {
  final output = Platform.environment['RYHZE_JOURNEY_REPORT'];
  if (output == null || output.isEmpty) {
    stderr.writeln('journey_driver: missing_report_destination');
    exit(1);
  }
  JourneyDriverResult result;
  try {
    final driver = await connectJourneyDriver<FlutterDriver>(
      connect: FlutterDriver.connect,
      close: (driver) => driver.close(),
    );
    result = await collectJourneyResult(
      request: () =>
          driver.requestData(null, timeout: const Duration(minutes: 3)),
      close: driver.close,
      allocation: () => allocation(driver),
    );
  } on TimeoutException {
    result = JourneyDriverResult.failed(JourneyFailure.connectTimeout);
  } catch (_) {
    // Do not put transport exceptions (which may include VM URLs) in evidence.
    result = JourneyDriverResult.failed(JourneyFailure.connectFailed);
  }
  try {
    await File(output).writeAsString(
      const JsonEncoder.withIndent('  ').convert(result.toJson()),
      flush: true,
    );
  } catch (_) {
    stderr.writeln('journey_driver: report_write_failed');
    exit(1);
  }
  // A timed-out close may leave transport work pending. The receipt is flushed
  // before terminating this driver process; app cleanup is a separate marker.
  exit(result.passed ? 0 : 1);
}

Future<Map<String, Object?>> allocation(FlutterDriver driver) async {
  if (driver is! VMServiceFlutterDriver) return {'available': false};
  final profile = await driver.serviceClient.getAllocationProfile(
    driver.appIsolate.id!,
  );
  return {
    'memory_usage': profile.memoryUsage?.toJson(),
    'instances_current': profile.members?.fold<int>(
      0,
      (n, c) => n + (c.instancesCurrent ?? 0),
    ),
    'bytes_current': profile.members?.fold<int>(
      0,
      (n, c) => n + (c.bytesCurrent ?? 0),
    ),
    'instances_accumulated': profile.members?.fold<int>(
      0,
      (n, c) => n + (c.instancesAccumulated ?? 0),
    ),
    'bytes_accumulated': profile.members?.fold<int>(
      0,
      (n, c) => n + (c.accumulatedSize ?? 0),
    ),
  };
}
