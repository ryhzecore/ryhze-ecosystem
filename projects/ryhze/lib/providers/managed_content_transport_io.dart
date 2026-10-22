import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import '../core/app_failure.dart';
import 'managed_content_transport.dart';

ManagedContentTransport createManagedContentTransport(Uri source) =>
    ManagedContentHttpTransport.approvedHttps(source);

class ManagedContentHttpTransport implements ManagedContentTransport {
  ManagedContentHttpTransport.approvedHttps(Uri source)
    : _source = source,
      timeout = const Duration(seconds: 15) {
    if (!isManagedContentHttpsSource(source)) _invalid();
  }

  /// Numeric-loopback fixture endpoint only; no DNS alias or external cleartext
  /// host can enter through this seam. This is not production provider approval.
  ManagedContentHttpTransport.loopbackForTesting(
    Uri source, {
    this.timeout = const Duration(seconds: 15),
  }) : _source = source {
    if (source.scheme != 'http' ||
        !source.hasAuthority ||
        !['127.0.0.1', '::1'].contains(source.host) ||
        source.userInfo.isNotEmpty ||
        source.hasQuery ||
        source.hasFragment ||
        timeout <= Duration.zero ||
        timeout > const Duration(seconds: 15)) {
      _invalid();
    }
  }

  final Uri _source;

  /// Bounds request/header and individual body waits. Awaited storage callbacks
  /// do not consume a network-idle deadline; their cancellation is owned by the
  /// caller and this operation always waits for them to actually settle.
  final Duration timeout;

  @override
  Future<ManagedContentTransferReceipt> transfer({
    required int expectedBytes,
    int offset = 0,
    String? strongEtag,
    required Future<void> Function(String? strongEtag) onResponse,
    required Future<void> Function(Uint8List bytes) onChunk,
    required CancellationToken cancellation,
  }) async {
    cancellation.check();
    if (expectedBytes < 1 ||
        expectedBytes > managedTransferMaximumBytes ||
        offset < 0 ||
        offset >= expectedBytes ||
        (offset == 0 && strongEtag != null) ||
        (offset > 0 &&
            (strongEtag == null || !isManagedContentStrongEtag(strongEtag)))) {
      _invalid();
    }

    // A fresh client prevents cookies, credentials, connections or proxy state
    // crossing transfers. Dart's normal TLS certificate verification remains.
    final client = HttpClient()
      ..autoUncompress = false
      ..connectionTimeout = timeout
      ..findProxy = (_) => 'DIRECT';
    HttpClientRequest? request;
    StreamIterator<List<int>>? body;
    Timer? timer;
    AppFailure? stopped;

    void stop(AppFailure failure) {
      stopped ??= failure;
      request?.abort(stopped);
      client.close(force: true);
    }

    void check() {
      cancellation.check();
      final failure = stopped;
      if (failure != null) throw failure;
    }

    void armDeadline() {
      timer?.cancel();
      timer = Timer(timeout, () => stop(const AppFailure(FailureKind.network)));
    }

    Future<void> deliver(Future<void> Function() callback) async {
      check();
      try {
        await callback();
      } on AppFailure {
        rethrow;
      } catch (_) {
        throw const AppFailure(FailureKind.storage);
      }
      check();
    }

    final removeCancellation = cancellation.onCancel(
      () => stop(const AppFailure(FailureKind.cancelled)),
    );
    try {
      check();
      armDeadline();
      final activeRequest = await client.getUrl(_source);
      request = activeRequest;
      check();
      activeRequest
        ..followRedirects = false
        ..persistentConnection = false;
      activeRequest.headers
        ..set(HttpHeaders.acceptHeader, 'application/octet-stream')
        ..set(HttpHeaders.acceptEncodingHeader, 'identity');
      if (offset > 0) {
        activeRequest.headers
          ..set(HttpHeaders.rangeHeader, 'bytes=$offset-${expectedBytes - 1}')
          ..set(HttpHeaders.ifRangeHeader, strongEtag!);
      }
      final response = await activeRequest.close();
      timer?.cancel();
      check();
      if (response.statusCode == 401 || response.statusCode == 403) {
        throw const AppFailure(FailureKind.denied);
      }
      if (response.statusCode == 429 || response.statusCode >= 500) {
        throw const AppFailure(FailureKind.network);
      }
      final encoding = response.headers.value(
        HttpHeaders.contentEncodingHeader,
      );
      final etag = response.headers.value(HttpHeaders.etagHeader);
      final responseTag = etag != null && isManagedContentStrongEtag(etag)
          ? etag
          : null;
      final remaining = expectedBytes - offset;
      if (response.statusCode != (offset == 0 ? 200 : 206) ||
          response.contentLength != remaining ||
          response.headers.value(HttpHeaders.transferEncodingHeader) != null ||
          (encoding != null && encoding.toLowerCase() != 'identity')) {
        _invalid();
      }
      final range = response.headers.value(HttpHeaders.contentRangeHeader);
      if (offset == 0) {
        if (range != null) _invalid();
      } else if (range != 'bytes $offset-${expectedBytes - 1}/$expectedBytes' ||
          responseTag != strongEtag) {
        _invalid();
      }

      body = StreamIterator(response);
      await deliver(() => onResponse(responseTag));
      var received = 0;
      while (true) {
        check();
        armDeadline();
        final hasNext = await body.moveNext();
        timer?.cancel();
        check();
        if (!hasNext) break;
        final chunk = body.current;
        if (chunk.length > remaining - received) _invalid();
        // Bound each sink call even if the HTTP implementation coalesces data.
        for (var start = 0; start < chunk.length;) {
          final end = start + managedTransferMaximumChunkBytes < chunk.length
              ? start + managedTransferMaximumChunkBytes
              : chunk.length;
          final part = Uint8List(end - start)
            ..setRange(0, end - start, chunk, start);
          await deliver(() => onChunk(part));
          received += part.length;
          start = end;
        }
      }
      if (received != remaining) _invalid();
      check();
      return ManagedContentTransferReceipt(
        totalBytes: expectedBytes,
        transferredBytes: received,
        strongEtag: responseTag,
      );
    } on AppFailure {
      check();
      rethrow;
    } on FormatException {
      check();
      throw const AppFailure(FailureKind.invalidData);
    } catch (_) {
      check();
      throw const AppFailure(FailureKind.network);
    } finally {
      timer?.cancel();
      removeCancellation();
      request?.abort();
      client.close(force: true);
      // No race against a timeout/cancel future: reach cleanup only after every
      // awaited HTTP/sink operation above has actually returned.
      try {
        await body?.cancel();
      } catch (_) {
        // The abort may have already reported a closed socket. No new work is
        // started and the original closed-vocabulary failure remains decisive.
      }
    }
  }
}

Never _invalid() => throw const AppFailure(FailureKind.invalidData);
