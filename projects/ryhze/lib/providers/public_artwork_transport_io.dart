import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import '../core/app_failure.dart';
import '../design/artwork_reference.dart';
import 'public_artwork_png.dart';
import 'public_artwork_transport.dart';

PublicArtworkTransport createPublicArtworkTransport() =>
    PublicArtworkHttpTransport();

class PublicArtworkHttpTransport implements PublicArtworkTransport {
  PublicArtworkHttpTransport()
    : _origin = Uri.parse('https://ryhze.com'),
      timeout = const Duration(seconds: 15),
      maxBytes = publicArtworkMaximumBytes;

  /// Numeric-loopback test seam only; production never takes a supplied origin.
  PublicArtworkHttpTransport.loopbackForTesting(
    Uri origin, {
    this.timeout = const Duration(seconds: 15),
    this.maxBytes = publicArtworkMaximumBytes,
  }) : _origin = origin {
    if (origin.scheme != 'http' ||
        !['127.0.0.1', '::1'].contains(origin.host) ||
        origin.userInfo.isNotEmpty ||
        origin.hasQuery ||
        origin.hasFragment ||
        (origin.path.isNotEmpty && origin.path != '/') ||
        timeout <= Duration.zero ||
        timeout > const Duration(seconds: 15) ||
        maxBytes < 1 ||
        maxBytes > publicArtworkMaximumBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  final Uri _origin;
  final Duration timeout;
  final int maxBytes;

  @override
  Future<Uint8List> fetch(
    ArtworkReference reference,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    final client = HttpClient()
      ..autoUncompress = false
      ..connectionTimeout = timeout
      ..findProxy = (_) => 'DIRECT';
    HttpClientRequest? request;
    final stopped = Completer<Uint8List>();
    void stop(AppFailure failure) {
      if (!stopped.isCompleted) stopped.completeError(failure);
      request?.abort(failure);
      client.close(force: true);
    }

    final remove = cancellation.onCancel(
      () => stop(const AppFailure(FailureKind.cancelled)),
    );
    final timer = Timer(
      timeout,
      () => stop(const AppFailure(FailureKind.network)),
    );
    Future<Uint8List> receive() async {
      request = await client.getUrl(_origin.replace(path: reference.path));
      cancellation.check();
      request!
        ..followRedirects = false
        ..persistentConnection = false;
      request!.headers
        ..set(HttpHeaders.acceptHeader, 'image/png')
        ..set(HttpHeaders.acceptEncodingHeader, 'identity');
      final response = await request!.close();
      cancellation.check();
      if (response.statusCode == 401 || response.statusCode == 403) {
        throw const AppFailure(FailureKind.denied);
      }
      if (response.statusCode == 429 || response.statusCode >= 500) {
        throw const AppFailure(FailureKind.network);
      }
      final encoding = response.headers.value(
        HttpHeaders.contentEncodingHeader,
      );
      if (response.statusCode != HttpStatus.ok ||
          response.headers.contentType?.mimeType.toLowerCase() != 'image/png' ||
          (encoding != null && encoding.toLowerCase() != 'identity') ||
          response.contentLength > maxBytes) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final bytes = BytesBuilder(copy: false);
      await for (final chunk in response) {
        cancellation.check();
        if (bytes.length + chunk.length > maxBytes) {
          throw const AppFailure(FailureKind.invalidData);
        }
        bytes.add(chunk);
      }
      cancellation.check();
      if (response.contentLength >= 0 &&
          bytes.length != response.contentLength) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final result = bytes.takeBytes();
      return result;
    }

    try {
      return await Future.any([receive(), stopped.future]);
    } on AppFailure {
      rethrow;
    } on FormatException {
      throw const AppFailure(FailureKind.invalidData);
    } catch (_) {
      cancellation.check();
      throw const AppFailure(FailureKind.network);
    } finally {
      timer.cancel();
      remove();
      client.close(force: true);
    }
  }
}
