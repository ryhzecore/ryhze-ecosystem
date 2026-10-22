import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_driver/flutter_driver.dart';

import 'journey_result.dart';
import 'transfer_journey_result.dart';

Future<void> main() async {
  final output = Platform.environment['RYHZE_JOURNEY_REPORT'];
  if (output == null || output.isEmpty) {
    stderr.writeln('transfer_journey_driver: missing_report_destination');
    exit(1);
  }
  FlutterDriver? driver;
  var result = JourneyDriverResult.failed(JourneyFailure.connectFailed);
  try {
    driver = await connectJourneyDriver<FlutterDriver>(
      connect: FlutterDriver.connect,
      close: (value) => value.close(),
    );
    try {
      final response = await driver
          .requestData(null, timeout: const Duration(seconds: 240))
          .timeout(const Duration(seconds: 245));
      result = parseTransferJourneyResponse(response);
    } on TimeoutException {
      result = JourneyDriverResult.failed(JourneyFailure.requestTimeout);
    } catch (_) {
      result = JourneyDriverResult.failed(JourneyFailure.requestFailed);
    }
  } on TimeoutException {
    result = JourneyDriverResult.failed(JourneyFailure.connectTimeout);
  } catch (_) {
    result = JourneyDriverResult.failed(JourneyFailure.connectFailed);
  } finally {
    if (driver != null) {
      try {
        await driver.close().timeout(const Duration(seconds: 10));
      } on TimeoutException {
        result.closeFailure = JourneyFailure.closeTimeout;
      } catch (_) {
        result.closeFailure = JourneyFailure.closeFailed;
      }
    }
  }
  try {
    await File(output).writeAsString(
      const JsonEncoder.withIndent('  ').convert(result.toJson()),
      flush: true,
    );
  } catch (_) {
    stderr.writeln('transfer_journey_driver: report_write_failed');
    exit(1);
  }
  exit(result.passed ? 0 : 1);
}
