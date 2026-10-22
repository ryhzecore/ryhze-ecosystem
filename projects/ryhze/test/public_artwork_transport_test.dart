import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/design/artwork_reference.dart';
import 'package:ryhze/providers/public_artwork_png.dart';
import 'package:ryhze/providers/public_artwork_decoder.dart';
import 'package:ryhze/providers/public_artwork_transport_io.dart';

import 'artwork_fixture.dart';

void main() {
  Matcher failure(FailureKind kind) =>
      throwsA(isA<AppFailure>().having((e) => e.kind, 'kind', kind));
  Future<HttpServer> serve(Future<void> Function(HttpRequest) handler) async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    server.listen((request) async {
      unawaited(
        request.response.done.then<void>((_) {}, onError: (Object _) {}),
      );
      try {
        await handler(request);
      } on SocketException {
        /* Deliberate abort. */
      } on HttpException {
        /* Deliberate abort. */
      }
    });
    addTearDown(() => server.close(force: true));
    return server;
  }

  PublicArtworkHttpTransport transport(
    HttpServer server, {
    int maximum = publicArtworkMaximumBytes,
    Duration timeout = const Duration(seconds: 15),
  }) => PublicArtworkHttpTransport.loopbackForTesting(
    Uri.parse('http://127.0.0.1:${server.port}'),
    maxBytes: maximum,
    timeout: timeout,
  );
  test('identity binds all fields and admits only public flat PNG paths', () {
    final reference = artworkRef();
    expect(reference.key, matches(RegExp(r'^[0-9a-f]{64}$')));
    expect(reference, artworkRef());
    expect(reference.key, isNot(artworkRef(0, 2).key));
    for (final path in [
      '/art/../a.png',
      '/art/a.PNG',
      'https://ryhze.com/art/a.png',
      '/art/a.png?token=x',
      '/art/a%20b.png',
      '/art/a.png\n',
      '/art/a/b.png',
    ]) {
      expect(
        () => ArtworkReference(
          itemId: 'a',
          revision: 1,
          path: path,
          note: 'Concept',
        ),
        failure(FailureKind.invalidData),
      );
    }
    expect(
      () => ArtworkReference(
        itemId: 'a',
        revision: -1,
        path: '/art/a.png',
        note: 'Concept',
      ),
      failure(FailureKind.invalidData),
    );
    expect(
      () => ArtworkReference(
        itemId: 'a',
        revision: 1,
        path: '/art/a.png',
        note: '\u0000',
      ),
      failure(FailureKind.invalidData),
    );
  });
  test(
    'test-only origin accepts numeric loopback without credentials or suffixes',
    () {
      for (final origin in [
        'https://ryhze.com',
        'http://localhost',
        'http://example.com',
        'http://user@127.0.0.1',
        'http://127.0.0.1/path',
        'http://127.0.0.1?x=1',
        'http://127.0.0.1#x',
      ]) {
        expect(
          () =>
              PublicArtworkHttpTransport.loopbackForTesting(Uri.parse(origin)),
          failure(FailureKind.invalidData),
        );
      }
    },
  );
  test('actual HTTP is anonymous, exact path, no cookie reuse', () async {
    var calls = 0;
    final bytes = artworkPng();
    final server = await serve((request) async {
      calls++;
      expect(request.uri.toString(), '/art/test-0.png');
      expect(request.headers.value(HttpHeaders.authorizationHeader), isNull);
      expect(request.headers.value(HttpHeaders.cookieHeader), isNull);
      expect(
        request.headers.value(HttpHeaders.acceptEncodingHeader),
        'identity',
      );
      request.response
        ..headers.contentType = ContentType('image', 'png')
        ..cookies.add(Cookie('ignored', 'test'))
        ..add(bytes);
      await request.response.close();
    });
    for (var i = 0; i < 2; ++i) {
      expect(
        await transport(server).fetch(artworkRef(), CancellationToken()),
        bytes,
      );
    }
    expect(calls, 2);
  });
  for (final status in [302, 401, 403, 404, 429, 500]) {
    test('HTTP $status is refused without redirects', () async {
      var calls = 0;
      final server = await serve((request) async {
        calls++;
        request.response
          ..statusCode = status
          ..headers.set(HttpHeaders.locationHeader, '/redirected');
        await request.response.close();
      });
      await expectLater(
        transport(server).fetch(artworkRef(), CancellationToken()),
        failure(
          status == 401 || status == 403
              ? FailureKind.denied
              : status == 429 || status >= 500
              ? FailureKind.network
              : FailureKind.invalidData,
        ),
      );
      expect(calls, 1);
    });
  }
  for (final mode in ['mime', 'encoding', 'length', 'chunked']) {
    test('rejects $mode outside bounded PNG transport contract', () async {
      final server = await serve((request) async {
        request.response.headers.contentType = ContentType(
          'image',
          mode == 'mime' ? 'jpeg' : 'png',
        );
        if (mode == 'encoding') {
          request.response.headers.set(
            HttpHeaders.contentEncodingHeader,
            'gzip',
          );
        }
        if (mode == 'length') request.response.contentLength = 100;
        request.response.add(List.filled(100, 0));
        await request.response.close();
      });
      await expectLater(
        transport(server, maximum: 80).fetch(artworkRef(), CancellationToken()),
        failure(FailureKind.invalidData),
      );
    });
  }
  test(
    'deadline bounds stalled headers and cancellation aborts active read',
    () async {
      final requests = <HttpRequest>[];
      final server = await serve((request) async {
        requests.add(request);
      });
      await expectLater(
        transport(
          server,
          timeout: const Duration(milliseconds: 50),
        ).fetch(artworkRef(), CancellationToken()),
        failure(FailureKind.network),
      );
      final token = CancellationToken();
      final read = transport(server).fetch(artworkRef(), token);
      final check = expectLater(read, failure(FailureKind.cancelled));
      while (requests.length < 2) {
        await Future<void>.delayed(const Duration(milliseconds: 5));
      }
      token.cancel();
      await check;
    },
  );
  test('PNG rejects dimensions, animation, CRC corruption, truncation and trailing data', () {
    expect(validatePublicArtworkPng(artworkPng()).width, 1);
    final badCrc = artworkPng()..[35] ^= 1;
    for (final bytes in [
      artworkPng(width: 0),
      artworkPng(width: 8193),
      artworkPng(width: 8192, height: 8192),
      artworkPng(animated: true),
      badCrc,
      Uint8List.fromList(artworkPng().sublist(0, 60)),
      Uint8List.fromList([...artworkPng(), 0]),
    ]) {
      expect(
        () => validatePublicArtworkPng(bytes),
        failure(FailureKind.invalidData),
      );
    }
  });
  test('actual Flutter decoder accepts pixels and rejects valid-CRC bad compressed bytes', () async {
    await validateDecodedPublicArtwork(artworkPng());
    final bad = artworkPng(corruptPixels: true);
    validatePublicArtworkPng(bad);
    await expectLater(
      validateDecodedPublicArtwork(bad),
      failure(FailureKind.invalidData),
    );
  });
}
