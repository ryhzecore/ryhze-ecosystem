import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/providers/managed_content_transport.dart';
import 'package:ryhze/providers/managed_content_transport_io.dart';

Matcher failure(FailureKind kind) =>
    throwsA(isA<AppFailure>().having((error) => error.kind, 'kind', kind));

Future<HttpServer> serve(Future<void> Function(HttpRequest) handler) async {
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
  server.listen((request) async {
    unawaited(request.response.done.then<void>((_) {}, onError: (Object _) {}));
    try {
      await handler(request);
    } on SocketException {
      // Intentional client abort during cancellation/negative cases.
    } on HttpException {
      // Intentional malformed/truncated response or client abort.
    }
  });
  addTearDown(() => server.close(force: true));
  return server;
}

ManagedContentHttpTransport transport(
  HttpServer server, {
  Duration timeout = const Duration(seconds: 15),
}) => ManagedContentHttpTransport.loopbackForTesting(
  Uri.parse('http://127.0.0.1:${server.port}/packages/exact.bin'),
  timeout: timeout,
);

Future<ManagedContentTransferReceipt> read(
  ManagedContentTransport transport, {
  int expectedBytes = 4,
  int offset = 0,
  String? strongEtag,
  CancellationToken? cancellation,
  Future<void> Function(String?)? onResponse,
  Future<void> Function(Uint8List)? onChunk,
}) => transport.transfer(
  expectedBytes: expectedBytes,
  offset: offset,
  strongEtag: strongEtag,
  onResponse: onResponse ?? (_) async {},
  onChunk: onChunk ?? (_) async {},
  cancellation: cancellation ?? CancellationToken(),
);

void main() {
  test('production source binds one HTTPS resource without credentials', () {
    expect(
      ManagedContentTransport.approvedHttps(
        Uri.parse('https://content.example.test/releases/package.bin'),
      ),
      isA<ManagedContentHttpTransport>(),
    );
    for (final value in [
      'http://content.example.test/payload',
      'file:///C:/payload',
      '/payload',
      'https://user:secret@content.example.test/payload',
      'https://content.example.test/payload?token=secret',
      'https://content.example.test/payload#fragment',
    ]) {
      expect(
        () => ManagedContentTransport.approvedHttps(Uri.parse(value)),
        failure(FailureKind.invalidData),
      );
    }
  });

  test(
    'fixture transport only accepts numeric loopback and bounded deadlines',
    () {
      for (final value in [
        'https://127.0.0.1/payload',
        'http://localhost/payload',
        'http://example.test/payload',
        'http://user@127.0.0.1/payload',
        'http://127.0.0.1/payload?token=secret',
        'http://127.0.0.1/payload#fragment',
      ]) {
        expect(
          () =>
              ManagedContentHttpTransport.loopbackForTesting(Uri.parse(value)),
          failure(FailureKind.invalidData),
        );
      }
      for (final timeout in [Duration.zero, const Duration(seconds: 16)]) {
        expect(
          () => ManagedContentHttpTransport.loopbackForTesting(
            Uri.parse('http://127.0.0.1/payload'),
            timeout: timeout,
          ),
          failure(FailureKind.invalidData),
        );
      }
    },
  );

  test('only bounded exact strong ETags can qualify a retained prefix', () {
    for (final tag in ['""', '"version-1"', '"a\\b"']) {
      expect(isManagedContentStrongEtag(tag), isTrue);
    }
    for (final tag in [
      'version-1',
      'W/"version-1"',
      '"white space"',
      '"line\nbreak"',
      '"trailing"\n',
      '"embedded"quote"',
      '"non-ascii-\u00e9"',
      '"${'a' * 255}"',
    ]) {
      expect(isManagedContentStrongEtag(tag), isFalse);
    }
  });

  test('request limits reject before contacting server', () async {
    var requests = 0;
    final server = await serve((request) async {
      requests++;
      await request.response.close();
    });
    final client = transport(server);
    for (final bytes in [0, -1, managedTransferMaximumBytes + 1]) {
      await expectLater(
        read(client, expectedBytes: bytes),
        failure(FailureKind.invalidData),
      );
    }
    for (final offset in [-1, 4, 5]) {
      await expectLater(
        read(client, offset: offset, strongEtag: '"v1"'),
        failure(FailureKind.invalidData),
      );
    }
    for (final tag in [null, 'W/"weak"', 'unquoted', '"bad\nheader"']) {
      await expectLater(
        read(client, offset: 1, strongEtag: tag),
        failure(FailureKind.invalidData),
      );
    }
    await expectLater(
      read(client, strongEtag: '"v1"'),
      failure(FailureKind.invalidData),
    );
    expect(requests, 0);
  });

  test(
    'real transfer is anonymous, bounded and fresh across requests',
    () async {
      final bytes = Uint8List.fromList(
        List.generate(600000, (index) => index % 251),
      );
      var calls = 0;
      final server = await serve((request) async {
        calls++;
        expect(request.uri.toString(), '/packages/exact.bin');
        expect(request.headers.value(HttpHeaders.authorizationHeader), isNull);
        expect(request.headers.value(HttpHeaders.cookieHeader), isNull);
        expect(request.headers.value(HttpHeaders.rangeHeader), isNull);
        expect(request.headers.value(HttpHeaders.ifRangeHeader), isNull);
        expect(
          request.headers.value(HttpHeaders.acceptEncodingHeader),
          'identity',
        );
        request.response
          ..contentLength = bytes.length
          ..headers.set(HttpHeaders.etagHeader, '"version-1"')
          ..cookies.add(Cookie('ignored', 'fixture'))
          ..add(bytes);
        await request.response.close();
      });
      final client = transport(server);
      for (var attempt = 0; attempt < 2; attempt++) {
        final received = BytesBuilder();
        var admitted = false;
        var activeSinks = 0;
        final receipt = await read(
          client,
          expectedBytes: bytes.length,
          onResponse: (tag) async {
            expect(tag, '"version-1"');
            admitted = true;
          },
          onChunk: (chunk) async {
            expect(admitted, isTrue);
            expect(
              chunk.length,
              inInclusiveRange(1, managedTransferMaximumChunkBytes),
            );
            expect(activeSinks++, 0);
            await Future<void>.delayed(Duration.zero);
            received.add(chunk);
            activeSinks--;
          },
        );
        expect(received.takeBytes(), bytes);
        expect(receipt.totalBytes, bytes.length);
        expect(receipt.transferredBytes, bytes.length);
        expect(receipt.strongEtag, '"version-1"');
      }
      expect(calls, 2);
    },
  );

  for (final tag in <String?>[null, 'W/"weak"']) {
    test('full transfer with $tag is usable but not resumable', () async {
      final server = await serve((request) async {
        request.response.contentLength = 4;
        if (tag != null) {
          request.response.headers.set(HttpHeaders.etagHeader, tag);
        }
        request.response.add([1, 2, 3, 4]);
        await request.response.close();
      });
      expect((await read(transport(server))).strongEtag, isNull);
    });
  }

  test(
    'resumption validates the exact remaining range before delivery',
    () async {
      final server = await serve((request) async {
        expect(request.headers.value(HttpHeaders.rangeHeader), 'bytes=2-5');
        expect(request.headers.value(HttpHeaders.ifRangeHeader), '"version-1"');
        request.response
          ..statusCode = HttpStatus.partialContent
          ..contentLength = 4
          ..headers.set(HttpHeaders.contentRangeHeader, 'bytes 2-5/6')
          ..headers.set(HttpHeaders.etagHeader, '"version-1"')
          ..add([3, 4, 5, 6]);
        await request.response.close();
      });
      final received = BytesBuilder();
      final receipt = await read(
        transport(server),
        expectedBytes: 6,
        offset: 2,
        strongEtag: '"version-1"',
        onChunk: (chunk) async => received.add(chunk),
      );
      expect(received.takeBytes(), [3, 4, 5, 6]);
      expect(receipt.totalBytes, 6);
      expect(receipt.transferredBytes, 4);
      expect(receipt.strongEtag, '"version-1"');
    },
  );

  for (final mode in [
    'full',
    'missing-range',
    'wrong-start',
    'wrong-end',
    'wrong-total',
    'unknown-total',
    'missing-etag',
    'changed-etag',
    'weak-etag',
    'length',
  ]) {
    test('resume refuses $mode before admitting any bytes', () async {
      final server = await serve((request) async {
        final range = switch (mode) {
          'wrong-start' => 'bytes 1-5/6',
          'wrong-end' => 'bytes 2-4/6',
          'wrong-total' => 'bytes 2-5/7',
          'unknown-total' => 'bytes 2-5/*',
          _ => 'bytes 2-5/6',
        };
        request.response
          ..statusCode = mode == 'full' ? 200 : 206
          ..contentLength = mode == 'length' ? 3 : 4;
        if (mode != 'missing-range') {
          request.response.headers.set(HttpHeaders.contentRangeHeader, range);
        }
        if (mode != 'missing-etag') {
          request.response.headers.set(
            HttpHeaders.etagHeader,
            mode == 'changed-etag'
                ? '"version-2"'
                : mode == 'weak-etag'
                ? 'W/"version-1"'
                : '"version-1"',
          );
        }
        request.response.add(mode == 'length' ? [1, 2, 3] : [1, 2, 3, 4]);
        await request.response.close();
      });
      var admitted = false;
      await expectLater(
        read(
          transport(server),
          expectedBytes: 6,
          offset: 2,
          strongEtag: '"version-1"',
          onResponse: (_) async {
            admitted = true;
          },
          onChunk: (_) async {
            fail('Invalid response reached the writer');
          },
        ),
        failure(FailureKind.invalidData),
      );
      expect(admitted, isFalse);
    });
  }

  for (final status in [301, 302, 307, 401, 403, 404, 416, 429, 500]) {
    test('HTTP $status is closed-vocabulary and never followed', () async {
      var calls = 0;
      final server = await serve((request) async {
        calls++;
        request.response
          ..statusCode = status
          ..headers.set(HttpHeaders.locationHeader, '/redirected');
        await request.response.close();
      });
      await expectLater(
        read(transport(server)),
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

  for (final mode in ['length', 'chunked', 'encoding', 'partial', 'range']) {
    test('full transfer refuses $mode before writer admission', () async {
      final server = await serve((request) async {
        if (mode != 'chunked') {
          request.response.contentLength = mode == 'length' ? 5 : 4;
        }
        if (mode == 'encoding') {
          request.response.headers.set(
            HttpHeaders.contentEncodingHeader,
            'gzip',
          );
        }
        if (mode == 'partial') request.response.statusCode = 206;
        if (mode == 'range') {
          request.response.headers.set(
            HttpHeaders.contentRangeHeader,
            'bytes 0-3/4',
          );
        }
        request.response.add(mode == 'length' ? [1, 2, 3, 4, 5] : [1, 2, 3, 4]);
        await request.response.close();
      });
      await expectLater(
        read(
          transport(server),
          onResponse: (_) async {
            fail('Invalid response admitted');
          },
        ),
        failure(FailureKind.invalidData),
      );
    });
  }

  test('pre-cancelled transfer cannot contact its endpoint', () async {
    var requests = 0;
    final server = await serve((request) async {
      requests++;
    });
    await expectLater(
      read(transport(server), cancellation: CancellationToken()..cancel()),
      failure(FailureKind.cancelled),
    );
    expect(requests, 0);
  });

  test('cancellation aborts a real stalled header wait', () async {
    final contacted = Completer<void>();
    final server = await serve((request) async {
      contacted.complete();
    });
    final token = CancellationToken();
    final operation = read(transport(server), cancellation: token);
    final checked = expectLater(operation, failure(FailureKind.cancelled));
    await contacted.future;
    token.cancel();
    await checked;
  });

  test('deadline terminates a real stalled header wait', () async {
    final server = await serve((request) async {});
    await expectLater(
      read(transport(server, timeout: const Duration(milliseconds: 80))),
      failure(FailureKind.network),
    );
  });

  test('body inactivity deadline aborts after a delivered prefix', () async {
    final server = await serve((request) async {
      final socket = await request.response.detachSocket(writeHeaders: false);
      addTearDown(socket.destroy);
      socket.write(
        'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n'
        'Connection: close\r\n\r\na',
      );
      await socket.flush();
    });
    var received = 0;
    await expectLater(
      read(
        transport(server, timeout: const Duration(milliseconds: 100)),
        onChunk: (bytes) async {
          received += bytes.length;
        },
      ),
      failure(FailureKind.network),
    );
    expect(received, 1);
  });

  test('an abruptly truncated body never reports a transfer receipt', () async {
    final server = await serve((request) async {
      final socket = await request.response.detachSocket(writeHeaders: false);
      socket.write(
        'HTTP/1.1 200 OK\r\nContent-Length: 4\r\n'
        'Connection: close\r\n\r\nab',
      );
      await socket.flush();
      await socket.close();
    });
    await expectLater(read(transport(server)), failure(FailureKind.network));
  });

  for (final callback in ['response', 'chunk']) {
    test(
      'cancellation waits for the active $callback callback to settle',
      () async {
        final entered = Completer<void>();
        final release = Completer<void>();
        addTearDown(() {
          if (!release.isCompleted) release.complete();
        });
        final server = await serve((request) async {
          request.response
            ..contentLength = 4
            ..add([1, 2, 3, 4]);
          await request.response.close();
        });
        final token = CancellationToken();
        var settled = false;
        var chunks = 0;
        Future<void> block() async {
          entered.complete();
          await release.future;
        }

        final operation =
            read(
              transport(server),
              cancellation: token,
              onResponse: (_) async {
                if (callback == 'response') await block();
              },
              onChunk: (_) async {
                chunks++;
                if (callback == 'chunk') await block();
              },
            ).whenComplete(() {
              settled = true;
            });
        final checked = expectLater(operation, failure(FailureKind.cancelled));
        await entered.future;
        token.cancel();
        await Future<void>.delayed(const Duration(milliseconds: 30));
        expect(settled, isFalse);
        expect(chunks, callback == 'response' ? 0 : 1);
        release.complete();
        await checked;
        expect(settled, isTrue);
      },
    );
  }

  test('network-idle clock excludes awaited storage work', () async {
    final server = await serve((request) async {
      request.response
        ..contentLength = 4
        ..add([1, 2, 3, 4]);
      await request.response.close();
    });
    final receipt = await read(
      transport(server, timeout: const Duration(milliseconds: 100)),
      onChunk: (_) async {
        await Future<void>.delayed(const Duration(milliseconds: 150));
      },
    );
    expect(receipt.transferredBytes, 4);
  });

  for (final callback in ['response', 'chunk']) {
    test(
      '$callback sink failures are preserved and cannot report completion',
      () async {
        final server = await serve((request) async {
          request.response
            ..contentLength = 4
            ..add([1, 2, 3, 4]);
          await request.response.close();
        });
        await expectLater(
          read(
            transport(server),
            onResponse: (_) async {
              if (callback == 'response') {
                throw const AppFailure(FailureKind.recovery);
              }
            },
            onChunk: (_) async {
              throw const AppFailure(FailureKind.recovery);
            },
          ),
          failure(FailureKind.recovery),
        );
      },
    );
  }

  test('unexpected sink exceptions become a safe storage failure', () async {
    final server = await serve((request) async {
      request.response
        ..contentLength = 4
        ..add([1, 2, 3, 4]);
      await request.response.close();
    });
    await expectLater(
      read(
        transport(server),
        onChunk: (_) async {
          throw StateError('private fixture data');
        },
      ),
      failure(FailureKind.storage),
    );
  });
}
