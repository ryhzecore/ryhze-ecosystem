import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import '../core/app_failure.dart';
import 'provider_request_policy.dart';
import 'public_catalog_transport.dart';

PublicCatalogTransport createPublicCatalogTransport() =>
    PublicCatalogHttpTransport();

class PublicCatalogHttpTransport implements PublicCatalogTransport {
  PublicCatalogHttpTransport()
    : _endpoint = Uri.parse('https://ryhze.com/api/discover'),
      timeout = const Duration(seconds: 10),
      maxBytes = maximumBytes,
      clock = DateTime.now;

  /// Explicit test seam: numeric loopback only, never a production URL override.
  PublicCatalogHttpTransport.loopbackForTesting(
    Uri endpoint, {
    this.timeout = const Duration(seconds: 10),
    this.maxBytes = maximumBytes,
    DateTime Function()? clock,
  }) : _endpoint = endpoint,
       clock = clock ?? DateTime.now {
    if (endpoint.scheme != 'http' ||
        !['127.0.0.1', '::1'].contains(endpoint.host) ||
        endpoint.userInfo.isNotEmpty ||
        endpoint.hasQuery ||
        endpoint.hasFragment ||
        endpoint.path != '/api/discover' ||
        timeout <= Duration.zero ||
        timeout > const Duration(seconds: 30) ||
        maxBytes < 1 ||
        maxBytes > maximumBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }

  static const maximumBytes = 1024 * 1024;
  final Uri _endpoint;
  final Duration timeout;
  final int maxBytes;
  final DateTime Function() clock;

  @override
  Future<Uint8List> fetch(CancellationToken cancellation) async {
    cancellation.check();
    // A new client per operation has no shared cookies, authentication state,
    // connection pool or cancellation lifetime with another caller.
    final client = HttpClient()
      ..autoUncompress = false
      ..connectionTimeout = timeout
      ..findProxy = (_) => 'DIRECT';
    final stopped = Completer<Uint8List>();
    HttpClientRequest? request;
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
      request = await client.getUrl(_endpoint);
      cancellation.check();
      request!
        ..followRedirects = false
        ..persistentConnection = false;
      request!.headers
        ..set(HttpHeaders.acceptHeader, 'application/json')
        ..set(HttpHeaders.acceptEncodingHeader, 'identity');
      final response = await request!.close();
      cancellation.check();
      if (response.statusCode == 429 || response.statusCode >= 500) {
        throw ProviderRequestFailure(
          const AppFailure(FailureKind.network),
          retryAfter: _retryAfter(
            response.headers.value(HttpHeaders.retryAfterHeader),
          ),
        );
      }
      if (response.statusCode == 401 || response.statusCode == 403) {
        throw const AppFailure(FailureKind.denied);
      }
      if (response.statusCode != HttpStatus.ok) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final type = response.headers.contentType;
      final encoding = response.headers.value(
        HttpHeaders.contentEncodingHeader,
      );
      final charset = type?.parameters['charset'];
      if (type?.mimeType.toLowerCase() != 'application/json' ||
          (charset != null && charset.toLowerCase() != 'utf-8') ||
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
      return bytes.takeBytes();
    }

    try {
      final bytes = await Future.any([receive(), stopped.future]);
      cancellation.check();
      return bytes;
    } on ProviderRequestFailure {
      cancellation.check();
      rethrow;
    } on AppFailure catch (failure) {
      cancellation.check();
      if (failure.kind == FailureKind.network) {
        throw ProviderRequestFailure(failure);
      }
      rethrow;
    } on FormatException {
      cancellation.check();
      throw const AppFailure(FailureKind.invalidData);
    } catch (_) {
      cancellation.check();
      throw const ProviderRequestFailure(AppFailure(FailureKind.network));
    } finally {
      remove();
      timer.cancel();
      client.close(force: true);
    }
  }

  Duration? _retryAfter(String? value) {
    if (value == null) return null;
    if (RegExp(r'^\d+$').hasMatch(value) && int.tryParse(value) == null) {
      return const Duration(seconds: 180);
    }
    final seconds = int.tryParse(value);
    if (seconds != null && seconds >= 0) {
      // Values beyond the policy's maximum delay budget must stop retries,
      // never wrap or shorten a server-requested wait.
      return Duration(seconds: seconds > 180 ? 180 : seconds);
    }
    try {
      final wait = HttpDate.parse(value).difference(clock().toUtc());
      return wait.isNegative ? Duration.zero : wait;
    } on FormatException {
      return null;
    }
  }
}
