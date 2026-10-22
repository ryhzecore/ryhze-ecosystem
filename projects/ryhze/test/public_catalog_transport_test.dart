import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/providers/provider_request_policy.dart';
import 'package:ryhze/providers/public_catalog_transport_io.dart';

Matcher failure(FailureKind kind) =>
    isA<AppFailure>().having((e) => e.kind, 'kind', kind);
void main() {
  Future<HttpServer> serve(Future<void> Function(HttpRequest) handler) async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((request) async {
      unawaited(
        request.response.done.then<void>((_) {}, onError: (Object _) {}),
      );
      try {
        await handler(request);
      } on SocketException {
        /* A deliberately aborted fixture connection. */
      } on HttpException {
        /* A deliberately aborted fixture response. */
      }
    });
    addTearDown(() => server.close(force: true));
    return server;
  }

  Uri endpoint(HttpServer server) =>
      Uri.parse('http://127.0.0.1:${server.port}/api/discover');

  test(
    'production cannot take arbitrary destinations; test seam is loopback only',
    () {
      for (final value in [
        'https://ryhze.com/api/discover',
        'http://localhost/api/discover',
        'http://example.com/api/discover',
        'http://127.0.0.1/other',
        'http://user@127.0.0.1/api/discover',
        'http://127.0.0.1/api/discover?token=x',
        'http://127.0.0.1/api/discover#fragment',
      ]) {
        expect(
          () => PublicCatalogHttpTransport.loopbackForTesting(Uri.parse(value)),
          throwsA(failure(FailureKind.invalidData)),
        );
      }
    },
  );
  test(
    'real HTTP GET is anonymous and does not retain response cookies',
    () async {
      var requests = 0;
      final server = await serve((request) async {
        requests++;
        expect(request.method, 'GET');
        expect(request.uri.path, '/api/discover');
        expect(request.headers.value(HttpHeaders.authorizationHeader), isNull);
        expect(request.headers.value(HttpHeaders.cookieHeader), isNull);
        expect(
          request.headers.value(HttpHeaders.acceptEncodingHeader),
          'identity',
        );
        request.response
          ..headers.contentType = ContentType.json
          ..cookies.add(Cookie('synthetic', 'must-not-return'))
          ..write('[]');
        await request.response.close();
      });
      final transport = PublicCatalogHttpTransport.loopbackForTesting(
        endpoint(server),
      );
      expect(utf8.decode(await transport.fetch(CancellationToken())), '[]');
      expect(utf8.decode(await transport.fetch(CancellationToken())), '[]');
      expect(requests, 2);
    },
  );
  test('redirects are refused without visiting target', () async {
    var requests = 0;
    final server = await serve((request) async {
      requests++;
      request.response
        ..statusCode = HttpStatus.found
        ..headers.set(HttpHeaders.locationHeader, '/redirect-target');
      await request.response.close();
    });
    await expectLater(
      PublicCatalogHttpTransport.loopbackForTesting(endpoint(server))
          .fetch(CancellationToken()),
      throwsA(failure(FailureKind.invalidData)),
    );
    expect(requests, 1);
  });
  test(
    'content type, encoding and known/chunked byte limits fail closed',
    () async {
      for (final mode in ['html', 'encoding', 'length', 'chunked']) {
        final server = await serve((request) async {
          final response = request.response;
          response.headers.contentType = mode == 'html'
              ? ContentType.html
              : ContentType.json;
          if (mode == 'encoding') {
            response.headers.set(HttpHeaders.contentEncodingHeader, 'gzip');
          }
          if (mode == 'length') response.contentLength = 8;
          response.write('[123456]');
          await response.close();
        });
        await expectLater(
          PublicCatalogHttpTransport.loopbackForTesting(
            endpoint(server),
            maxBytes: 4,
          ).fetch(CancellationToken()),
          throwsA(failure(FailureKind.invalidData)),
        );
      }
    },
  );
  test(
    '429 has bounded Retry-After and 403 is not retriable authentication',
    () async {
      final server = await serve((request) async {
        request.response
          ..statusCode = 429
          ..headers.set(
            HttpHeaders.retryAfterHeader,
            '999999999999999999999999',
          );
        await request.response.close();
      });
      await expectLater(
        PublicCatalogHttpTransport.loopbackForTesting(endpoint(server))
            .fetch(CancellationToken()),
        throwsA(
          isA<ProviderRequestFailure>()
              .having((e) => e.failure.kind, 'kind', FailureKind.network)
              .having(
                (e) => e.retryAfter,
                'retry bound',
                const Duration(seconds: 180),
              ),
        ),
      );
      final denied = await serve((request) async {
        request.response.statusCode = 403;
        await request.response.close();
      });
      await expectLater(
        PublicCatalogHttpTransport.loopbackForTesting(endpoint(denied))
            .fetch(CancellationToken()),
        throwsA(failure(FailureKind.denied)),
      );
    },
  );
  test('timeout bounds a response that never finishes', () async {
    final release = Completer<void>();
    addTearDown(() {
      if (!release.isCompleted) release.complete();
    });
    final server = await serve((request) async {
      request.response.headers.contentType = ContentType.json;
      request.response.write('[');
      await request.response.flush();
      await release.future;
      await request.response.close();
    });
    await expectLater(
      PublicCatalogHttpTransport.loopbackForTesting(
        endpoint(server),
        timeout: const Duration(milliseconds: 100),
      ).fetch(CancellationToken()).timeout(const Duration(seconds: 3)),
      throwsA(
        isA<ProviderRequestFailure>().having(
          (e) => e.failure.kind,
          'kind',
          FailureKind.network,
        ),
      ),
    );
  });
  test('cancel aborts body consumption promptly and leaves another request independent', () async {
    final started = Completer<void>(), release = Completer<void>();
    addTearDown(() {
      if (!release.isCompleted) release.complete();
    });
    var count = 0;
    final server = await serve((request) async {
      request.response.headers.contentType = ContentType.json;
      if (++count == 1) {
        request.response.write('[');
        await request.response.flush();
        started.complete();
        await release.future;
        request.response.write(']');
      } else {
        request.response.write('[]');
      }
      await request.response.close();
    });
    final transport = PublicCatalogHttpTransport.loopbackForTesting(
      endpoint(server),
    );
    final token = CancellationToken();
    final pending = transport.fetch(token);
    final expectation = expectLater(
      pending.timeout(const Duration(seconds: 3)),
      throwsA(failure(FailureKind.cancelled)),
    );
    await started.future.timeout(const Duration(seconds: 3));
    token.cancel();
    await expectation;
    expect(utf8.decode(await transport.fetch(CancellationToken())), '[]');
    release.complete();
  });
}
