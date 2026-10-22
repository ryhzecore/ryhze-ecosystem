import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import '../../core/app_failure.dart';
import 'apple_resource_policy.dart';

enum AppleNetworkFailure {
  denied,
  cancelled,
  timeout,
  unavailable,
  invalidData,
}

/// Safe, bounded public-resource reads. Provider authentication belongs to its
/// own adapter; neither arbitrary headers nor credentials enter this interface.
class AppleNetworkException implements Exception {
  const AppleNetworkException(this.kind);
  final AppleNetworkFailure kind;

  String get message => switch (kind) {
    AppleNetworkFailure.denied => 'This network destination is not supported.',
    AppleNetworkFailure.cancelled => 'The request was cancelled.',
    AppleNetworkFailure.timeout => 'The request timed out. Try again.',
    AppleNetworkFailure.unavailable =>
      'The resource is unavailable. Try again.',
    AppleNetworkFailure.invalidData => 'The resource could not be validated.',
  };

  @override
  String toString() => message;
}

class AppleHttpResponse {
  const AppleHttpResponse({
    required this.statusCode,
    required this.contentLength,
    required this.mimeType,
    required this.bytes,
    this.contentEncoding,
  });

  final int statusCode;
  final int contentLength;
  final String? mimeType;
  final String? contentEncoding;
  final Stream<List<int>> bytes;
}

/// Each instance owns exactly one request; closing must abort connection and
/// response work, including cancellation before response headers arrive.
abstract interface class AppleHttpTransport {
  Future<AppleHttpResponse> open(Uri uri);
  void close();
}

class IoAppleHttpTransport implements AppleHttpTransport {
  final HttpClient _client = HttpClient()..autoUncompress = false;

  @override
  Future<AppleHttpResponse> open(Uri uri) async {
    final request = await _client.getUrl(uri);
    request.followRedirects = false;
    request.headers.set(HttpHeaders.acceptEncodingHeader, 'identity');
    final response = await request.close();
    return AppleHttpResponse(
      statusCode: response.statusCode,
      contentLength: response.contentLength,
      mimeType: response.headers.contentType?.mimeType,
      contentEncoding: response.headers.value(
        HttpHeaders.contentEncodingHeader,
      ),
      bytes: response,
    );
  }

  @override
  void close() => _client.close(force: true);
}

class AppleNetworkClient {
  AppleNetworkClient({
    required this.policy,
    AppleHttpTransport Function()? transportFactory,
    this.maximumBytes = 2 * 1024 * 1024,
    this.timeout = const Duration(seconds: 20),
  }) : _transportFactory = transportFactory ?? IoAppleHttpTransport.new {
    if (maximumBytes < 1 ||
        maximumBytes > 16 * 1024 * 1024 ||
        timeout <= Duration.zero ||
        timeout > const Duration(minutes: 1)) {
      throw ArgumentError('Invalid network resource budget.');
    }
  }

  final AppleResourcePolicy policy;
  final AppleHttpTransport Function() _transportFactory;
  final int maximumBytes;
  final Duration timeout;

  /// Only returns a complete validated response. Redirects never inherit trust.
  /// No retry is automatic: account/lifecycle owners decide whether to retry.
  Future<Uint8List> read(
    Uri uri, {
    CancellationToken? cancellation,
    Set<String> acceptedMimeTypes = const {'application/json'},
  }) async {
    if (!policy.allowsNetworkUri(uri)) {
      throw const AppleNetworkException(AppleNetworkFailure.denied);
    }
    if (cancellation?.isCancelled ?? false) {
      throw const AppleNetworkException(AppleNetworkFailure.cancelled);
    }
    if (acceptedMimeTypes.isEmpty ||
        acceptedMimeTypes.any(
          (type) => !const {
            'application/json',
            'image/png',
            'image/jpeg',
            'image/webp',
          }.contains(type),
        )) {
      throw const AppleNetworkException(AppleNetworkFailure.denied);
    }
    AppleHttpTransport? transport;
    void Function()? removeCancellation;
    try {
      transport = _transportFactory();
      final activeTransport = transport;
      final work = _readComplete(activeTransport, uri, acceptedMimeTypes);
      final cancelled = Completer<void>();
      removeCancellation = cancellation?.onCancel(() {
        if (!cancelled.isCompleted) cancelled.complete();
      });
      final result = cancellation == null
          ? work
          : Future.any<Uint8List>([
              work,
              cancelled.future.then<Uint8List>((_) {
                throw const AppleNetworkException(
                  AppleNetworkFailure.cancelled,
                );
              }),
            ]);
      return await result.timeout(
        timeout,
        onTimeout: () {
          throw const AppleNetworkException(AppleNetworkFailure.timeout);
        },
      );
    } on AppleNetworkException {
      rethrow;
    } catch (_) {
      // Never expose socket errors, remote bodies, paths or response headers.
      throw const AppleNetworkException(AppleNetworkFailure.unavailable);
    } finally {
      removeCancellation?.call();
      transport?.close();
    }
  }

  Future<Uint8List> _readComplete(
    AppleHttpTransport transport,
    Uri uri,
    Set<String> acceptedMimeTypes,
  ) async {
    final response = await transport.open(uri);
    if (response.statusCode != HttpStatus.ok) {
      throw const AppleNetworkException(AppleNetworkFailure.unavailable);
    }
    if (response.contentLength < -1 ||
        response.contentLength > maximumBytes ||
        !acceptedMimeTypes.contains(response.mimeType) ||
        (response.contentEncoding != null &&
            response.contentEncoding != 'identity')) {
      throw const AppleNetworkException(AppleNetworkFailure.invalidData);
    }
    final builder = BytesBuilder(copy: false);
    await for (final chunk in response.bytes) {
      if (chunk.length > maximumBytes - builder.length) {
        throw const AppleNetworkException(AppleNetworkFailure.invalidData);
      }
      builder.add(chunk);
    }
    if (response.contentLength >= 0 &&
        response.contentLength != builder.length) {
      throw const AppleNetworkException(AppleNetworkFailure.invalidData);
    }
    return builder.takeBytes();
  }
}
