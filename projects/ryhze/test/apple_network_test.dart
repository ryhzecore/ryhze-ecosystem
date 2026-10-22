import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/platform/apple/apple_network.dart';
import 'package:ryhze/platform/apple/apple_resource_policy.dart';

class _Transport implements AppleHttpTransport {
  _Transport(this.response);
  final Future<AppleHttpResponse> response;
  int opened = 0;
  bool closed = false;
  @override
  Future<AppleHttpResponse> open(Uri uri) {
    opened++;
    return response;
  }

  @override
  void close() => closed = true;
}

class _TrackedCancellation extends CancellationToken {
  int listeners = 0;
  @override
  void Function() onCancel(void Function() listener) {
    listeners++;
    final remove = super.onCancel(listener);
    var removed = false;
    return () {
      if (removed) return;
      removed = true;
      listeners--;
      remove();
    };
  }
}

AppleHttpResponse _response({
  int status = 200,
  int length = 2,
  String? mime = 'application/json',
  String? encoding,
  Stream<List<int>>? bytes,
}) => AppleHttpResponse(
  statusCode: status,
  contentLength: length,
  mimeType: mime,
  contentEncoding: encoding,
  bytes: bytes ?? Stream.value([123, 125]),
);

void main() {
  final uri = Uri.parse('https://catalog.example.org/items');
  final policy = AppleResourcePolicy(allowedHosts: ['catalog.example.org']);
  Matcher failure(AppleNetworkFailure kind) => isA<AppleNetworkException>()
      .having((error) => error.kind, 'safe failure category', kind);

  test('complete bounded response is returned and transport closed', () async {
    final transport = _Transport(Future.value(_response()));
    final client = AppleNetworkClient(
      policy: policy,
      transportFactory: () => transport,
    );
    expect(await client.read(uri), [123, 125]);
    expect(transport.opened, 1);
    expect(transport.closed, isTrue);
  });

  test('finished requests release shared cancellation listeners', () async {
    final cancellation = _TrackedCancellation();
    for (final status in [200, 503]) {
      final client = AppleNetworkClient(
        policy: policy,
        transportFactory: () =>
            _Transport(Future.value(_response(status: status))),
      );
      if (status == 200) {
        expect(await client.read(uri, cancellation: cancellation), [123, 125]);
      } else {
        await expectLater(
          client.read(uri, cancellation: cancellation),
          throwsA(failure(AppleNetworkFailure.unavailable)),
        );
      }
      expect(cancellation.listeners, 0);
    }
    cancellation.cancel();
  });

  test(
    'untrusted destinations and cancellation are denied before IO',
    () async {
      var creations = 0;
      final client = AppleNetworkClient(
        policy: policy,
        transportFactory: () {
          creations++;
          return _Transport(Future.value(_response()));
        },
      );
      for (final target in [
        'http://catalog.example.org/items',
        'https://catalog.example.org.attacker.test/items',
        'https://user:password@catalog.example.org/items',
        'https://catalog.example.org/items?access_token=fixture',
        'file:///tmp/content',
        'https://127.0.0.1/',
      ]) {
        await expectLater(
          client.read(Uri.parse(target)),
          throwsA(failure(AppleNetworkFailure.denied)),
        );
      }
      final cancellation = CancellationToken()..cancel();
      await expectLater(
        client.read(uri, cancellation: cancellation),
        throwsA(failure(AppleNetworkFailure.cancelled)),
      );
      expect(creations, 0);
    },
  );

  test('redirects do not receive another request or publish bytes', () async {
    final transport = _Transport(Future.value(_response(status: 302)));
    final client = AppleNetworkClient(
      policy: policy,
      transportFactory: () => transport,
    );
    await expectLater(
      client.read(uri),
      throwsA(failure(AppleNetworkFailure.unavailable)),
    );
    expect(transport.opened, 1);
    expect(transport.closed, isTrue);
  });

  test('unknown lengths remain bounded while streaming', () async {
    final transport = _Transport(
      Future.value(
        _response(
          length: -1,
          bytes: Stream.fromIterable([
            [1, 2],
            [3, 4],
          ]),
        ),
      ),
    );
    final client = AppleNetworkClient(
      policy: policy,
      maximumBytes: 3,
      transportFactory: () => transport,
    );
    await expectLater(
      client.read(uri),
      throwsA(failure(AppleNetworkFailure.invalidData)),
    );
    expect(transport.closed, isTrue);
  });

  test('truncated and malformed payload metadata fail safely', () async {
    for (final response in [
      _response(length: 3),
      _response(length: -2),
      _response(length: 100),
      _response(mime: 'text/html'),
      _response(mime: 'image/svg+xml'),
      _response(mime: null),
      _response(encoding: 'gzip'),
    ]) {
      final transport = _Transport(Future.value(response));
      final client = AppleNetworkClient(
        policy: policy,
        maximumBytes: 10,
        transportFactory: () => transport,
      );
      await expectLater(
        client.read(uri),
        throwsA(failure(AppleNetworkFailure.invalidData)),
      );
      expect(transport.closed, isTrue);
    }
  });

  test('cancel aborts a request waiting for headers', () async {
    final transport = _Transport(Completer<AppleHttpResponse>().future);
    final cancellation = CancellationToken();
    final client = AppleNetworkClient(
      policy: policy,
      transportFactory: () => transport,
    );
    final read = client.read(uri, cancellation: cancellation);
    final assertion = expectLater(
      read,
      throwsA(failure(AppleNetworkFailure.cancelled)),
    );
    cancellation.cancel();
    await assertion;
    expect(transport.closed, isTrue);
  });

  test('cancel aborts a request waiting for body data', () async {
    final stream = StreamController<List<int>>();
    final transport = _Transport(
      Future.value(_response(length: -1, bytes: stream.stream)),
    );
    final cancellation = CancellationToken();
    final client = AppleNetworkClient(
      policy: policy,
      transportFactory: () => transport,
    );
    final read = client.read(uri, cancellation: cancellation);
    final assertion = expectLater(
      read,
      throwsA(failure(AppleNetworkFailure.cancelled)),
    );
    await Future<void>.delayed(Duration.zero);
    cancellation.cancel();
    await assertion;
    expect(transport.closed, isTrue);
    await stream.close();
  });

  test('total timeout closes transport even before headers', () async {
    final transport = _Transport(Completer<AppleHttpResponse>().future);
    final client = AppleNetworkClient(
      policy: policy,
      timeout: const Duration(milliseconds: 5),
      transportFactory: () => transport,
    );
    await expectLater(
      client.read(uri),
      throwsA(failure(AppleNetworkFailure.timeout)),
    );
    expect(transport.closed, isTrue);
  });

  test(
    'exceptions and remote details never enter failure display text',
    () async {
      final client = AppleNetworkClient(
        policy: policy,
        transportFactory: () {
          throw StateError('private-token and account path');
        },
      );
      try {
        await client.read(uri);
        fail('Expected safe failure');
      } on AppleNetworkException catch (error) {
        expect(error.kind, AppleNetworkFailure.unavailable);
        expect(error.toString(), isNot(contains('private-token')));
        expect(error.toString(), isNot(contains('account path')));
      }
    },
  );

  test('active content types cannot be opted in by a caller', () async {
    final client = AppleNetworkClient(policy: policy);
    await expectLater(
      client.read(uri, acceptedMimeTypes: {'text/html'}),
      throwsA(failure(AppleNetworkFailure.denied)),
    );
  });
}
