// Test-only authority and filesystem model. Loopback exercises real HTTP;
// Windows native suites independently establish actual file-handle behavior.
// No fixture below is composed into application or provider runtime code.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_transfer_backend.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_admission.dart';
import 'package:ryhze/features/installations/managed_content_manifest.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_transfer_repository.dart';
import 'package:ryhze/features/installations/managed_transfer_service.dart';
import 'package:ryhze/features/installations/managed_transfer_source.dart';
import 'package:ryhze/features/jobs/job_coordinator.dart';
import 'package:ryhze/features/jobs/job_repository.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/providers/managed_content_transport.dart';
import 'package:ryhze/providers/managed_content_transport_io.dart';

const transferScope = AccountScope.verified('test-only-issuer', 'ExactAccount');
final transferNow = DateTime.utc(2026, 10, 12);
String digest(List<int> bytes) => crypto.sha256.convert(bytes).toString();

class TransferFixture {
  TransferFixture._(this.server, this.payload) {
    session.replace(transferScope);
    authority = TransferAuthority(
      manifestBytes,
      server == null
          ? const _ForbiddenWidgetTransport()
          : ManagedContentHttpTransport.loopbackForTesting(
              Uri.parse('http://127.0.0.1:${server!.port}/approved-content'),
            ),
    );
    jobs = JobRepository(store);
    repository = ManagedTransferRepository(
      store,
      session,
      deviceId: backend.deviceId,
    );
    service = ManagedTransferService(
      repository: repository,
      backend: backend,
      jobs: jobs,
      session: session,
      authority: authority,
      now: () => transferNow,
    );
    coordinator = JobCoordinator(jobs, {
      ManagedTransferService.jobKind: service,
      ManagedTransferService.discardKind: service,
    });
    session.addListener(coordinator.cancelActive);
    server?.listen((request) async {
      requests++;
      requestRanges.add(request.headers.value(HttpHeaders.rangeHeader));
      unawaited(
        request.response.done.then<void>((_) {}, onError: (Object _) {}),
      );
      try {
        if (onRequest != null) {
          await onRequest!(request);
        } else {
          await respond(request);
        }
      } on SocketException {
        // Deliberate client cancellation.
      } on HttpException {
        // Deliberate client cancellation or truncated fixture response.
      }
    });
  }

  static Future<TransferFixture> create({int size = 600000}) async =>
      TransferFixture._(
        await HttpServer.bind(InternetAddress.loopbackIPv4, 0),
        Uint8List.fromList(List.generate(size, (index) => index % 251)),
      );

  /// Widget tests seed local records and never transfer bytes. Construct every
  /// store future and backend completer in the caller's fake-async zone instead
  /// of leaking real-zone futures out of WidgetTester.runAsync.
  static TransferFixture withoutNetwork({int size = 128}) => TransferFixture._(
    null,
    Uint8List.fromList(List.generate(size, (index) => index % 251)),
  );

  final HttpServer? server;
  final Uint8List payload;
  final store = TransferStore();
  final session = SessionController();
  final backend = TransferBackend();
  late final TransferAuthority authority;
  late final ManagedTransferRepository repository;
  late final ManagedTransferService service;
  late final JobRepository jobs;
  late final JobCoordinator coordinator;
  final sockets = <Socket>[];
  int requests = 0;
  final requestRanges = <String?>[];
  String? etag = '"fixture-version-1"';
  Future<void> Function(HttpRequest)? onRequest;

  Uint8List get manifestBytes => Uint8List.fromList(
    utf8.encode(
      jsonEncode({
        'version': 1,
        'provider': 'test-provider',
        'item': 'ExactGame',
        'namespace': 'ExactNamespace',
        'packageId': 'test-package',
        'packageVersion': '1',
        'payloadSize': payload.length,
        'payloadSha256': digest(payload),
        'files': [
          {
            'path': 'payload.bin',
            'size': payload.length,
            'sha256': digest(payload),
            'executable': false,
          },
        ],
      }),
    ),
  );

  ManagedStageIntent get intent => ManagedStageIntent(
    operationId: 'a' * 64,
    item: ProviderItemRef(
      'test-provider',
      'ExactGame',
      namespace: 'ExactNamespace',
    ),
    packageId: 'test-package',
    version: '1',
    manifestSha256: digest(manifestBytes),
    sourceRelative: 'transfer-0/${'a' * 64}-0.bin',
    expectedSize: payload.length,
    expectedSha256: digest(payload),
    slot: 0,
  );

  Future<JobRecord> prepare() =>
      service.prepare(intent, CancellationToken(), coordinator: coordinator);
  Future<void> run() => coordinator.drain(session.scope);

  Future<void> respond(
    HttpRequest request, {
    int offset = 0,
    Uint8List? bytes,
  }) async {
    final data = bytes ?? payload;
    request.response
      ..statusCode = offset == 0 ? 200 : 206
      ..contentLength = data.length - offset;
    if (etag != null) {
      request.response.headers.set(HttpHeaders.etagHeader, etag!);
    }
    if (offset > 0) {
      request.response.headers.set(
        HttpHeaders.contentRangeHeader,
        'bytes $offset-${data.length - 1}/${data.length}',
      );
    }
    request.response.add(data.sublist(offset));
    await request.response.close();
  }

  Future<void> respondPrefix(HttpRequest request, int count) async {
    final socket = await request.response.detachSocket(writeHeaders: false);
    sockets.add(socket);
    socket.write(
      'HTTP/1.1 200 OK\r\nContent-Length: ${payload.length}\r\n'
      '${etag == null ? '' : 'ETag: $etag\r\n'}Connection: close\r\n\r\n',
    );
    socket.add(payload.sublist(0, count));
    await socket.flush();
    // Intentionally keep the remainder pending until client cancellation.
  }

  Future<void> close() async {
    store.fail = false;
    backend.failDrain = false;
    for (final gate in [
      backend.appendGate,
      backend.drainGate,
      authority.resolveGate,
    ]) {
      if (gate != null && !gate.isCompleted) gate.complete();
    }
    service.cancelPending();
    await coordinator.close();
    await service.close();
    for (final socket in sockets) {
      socket.destroy();
    }
    await server?.close(force: true);
    service.dispose();
    repository.dispose();
    jobs.dispose();
    session.dispose();
    await store.close();
  }
}

class _ForbiddenWidgetTransport implements ManagedContentTransport {
  const _ForbiddenWidgetTransport();

  @override
  Future<ManagedContentTransferReceipt> transfer({
    required int expectedBytes,
    int offset = 0,
    String? strongEtag,
    required Future<void> Function(String? strongEtag) onResponse,
    required Future<void> Function(Uint8List bytes) onChunk,
    required CancellationToken cancellation,
  }) async =>
      throw StateError('Widget fixture must not transfer network bytes');
}

class TransferStore extends MemoryTransactionStore {
  // Tests assert transaction sequencing; JournalStore tests prove durability.
  @override
  bool get durable => true;
  bool fail = false;
  bool failAfterNextCommit = false;
  void Function()? afterCommit;
  @override
  Future<void> transact(void Function(StoreData) update) async {
    if (fail) throw const AppFailure(FailureKind.storage);
    await super.transact(update);
    if (failAfterNextCommit) {
      failAfterNextCommit = false;
      throw const AppFailure(FailureKind.storage);
    }
    final callback = afterCommit;
    afterCommit = null;
    callback?.call();
  }
}

class TransferAuthority implements ManagedContentTransferAuthority {
  TransferAuthority(this.normalizedBytes, this.transport);
  final Uint8List normalizedBytes;
  final ManagedContentTransport transport;
  @override
  bool available = true;
  @override
  final authority = 'test-only-content-authority';
  @override
  final policy = 'test-only-policy';
  @override
  int generation = 1;
  final listeners = <void Function()>[];
  ManagedContentGrant? lastGrant;
  String sourceKey = 'test-only-content-v1';
  bool sourceFixture = false;
  bool wrongSourceScope = false;
  bool wrongGrantScope = false;
  bool wrongSourceArtifact = false;
  Completer<void>? resolveGate;
  final resolveEntered = Completer<void>();
  @override
  void addInvalidationListener(void Function() listener) =>
      listeners.add(listener);
  @override
  void removeInvalidationListener(void Function() listener) =>
      listeners.remove(listener);
  void revoke() {
    available = false;
    generation++;
    for (final listener in List.of(listeners)) {
      listener();
    }
  }

  ManagedManifestProvenance provenance({bool wrong = false}) =>
      ManagedManifestProvenance.create(
        authority: authority,
        policy: policy,
        sourceManifestSha256: (wrong ? 'e' : 'f') * 64,
        normalizedBytes: normalizedBytes,
        observedAt: transferNow,
        expiresAt: transferNow.add(const Duration(minutes: 4)),
      );

  @override
  Future<ManagedContentGrant> authorize(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    // A synthetic trusted-adapter assertion exercises admission's positive
    // branch only inside this test fixture. It establishes no real account.
    return lastGrant = ManagedContentGrant(
      scope: wrongGrantScope
          ? const AccountScope.verified('test-only-issuer', 'Other')
          : request.scope,
      sessionGeneration: request.sessionGeneration,
      deviceId: request.deviceId,
      provenance: provenance(),
      state: AccessState.verified,
      fixture: false,
    );
  }

  @override
  Future<ManagedTransferSource> resolveTransfer(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    if (!resolveEntered.isCompleted) resolveEntered.complete();
    await resolveGate?.future;
    cancellation.check();
    return ManagedTransferSource(
      request: wrongSourceScope
          ? ManagedContentAccessRequest(
              const AccountScope.verified('test-only-issuer', 'Other'),
              request.sessionGeneration,
              request.deviceId,
              request.intent,
            )
          : request,
      provenance: wrongSourceArtifact
          ? provenance(wrong: true)
          : lastGrant!.provenance,
      sourceKey: sourceKey,
      transport: transport,
      fixture: sourceFixture,
    );
  }
}

/// Exact identity/offset/hash assertions catch misuse by orchestration, without
/// substituting for the native handle, reparse-point or disk-space suites.
class TransferBackend implements ContentTransferBackend {
  @override
  final deviceId = '${'1' * 16}:${'2' * 32}';
  final directoryIdentity = '${'1' * 16}:${'4' * 32}';
  final files = <String, Uint8List>{};
  final identities = <String, String>{};
  final discarded = <String>[];
  int begins = 0, appends = 0, inspections = 0, drains = 0, cancels = 0;
  int _identity = 16;
  String? writer;
  bool failDrain = false;
  bool throwAfterFinish = false;
  Completer<void>? appendGate, drainGate;
  Completer<void>? _appendSettled;
  final appendEntered = Completer<void>();
  final appended = Completer<void>();
  void Function()? beforeBegin;

  ContentTransferReceipt receipt(
    ContentTransferRequest request,
    ContentTransferState state,
  ) {
    final data = files[request.sourceRelative];
    return ContentTransferReceipt(
      state: state,
      rootIdentity: deviceId,
      directoryIdentity: state == ContentTransferState.missing
          ? request.directoryIdentity
          : directoryIdentity,
      sourceIdentity: state == ContentTransferState.missing
          ? ''
          : identities[request.sourceRelative] ?? request.sourceIdentity,
      sourceRelative: request.sourceRelative,
      observedBytes: state == ContentTransferState.missing
          ? 0
          : data?.length ?? request.observedBytes,
      observedSha256:
          state == ContentTransferState.writing ||
              state == ContentTransferState.missing
          ? ''
          : data == null
          ? request.observedSha256
          : digest(data),
    );
  }

  void checkIdentity(ContentTransferRequest request) {
    if (request.rootIdentity.isNotEmpty && request.rootIdentity != deviceId ||
        request.directoryIdentity.isNotEmpty &&
            request.directoryIdentity != directoryIdentity ||
        request.sourceIdentity.isNotEmpty &&
            request.sourceIdentity != identities[request.sourceRelative]) {
      throw const AppFailure(FailureKind.recovery);
    }
  }

  void checkExact(ContentTransferRequest request, {bool writing = false}) {
    checkIdentity(request);
    final data = files[request.sourceRelative];
    if (data == null ||
        request.observedBytes != data.length ||
        (!writing && request.observedSha256 != digest(data))) {
      throw const AppFailure(FailureKind.recovery);
    }
  }

  @override
  Future<ContentTransferReceipt> begin(
    ContentTransferRequest request,
    CancellationToken cancellation, {
    ContentTransferRequest? resume,
  }) async {
    cancellation.check();
    beforeBegin?.call();
    if (writer != null || files.containsKey(request.sourceRelative)) {
      throw const AppFailure(FailureKind.conflict);
    }
    var bytes = Uint8List(0);
    if (resume != null) {
      checkExact(resume);
      bytes = Uint8List.fromList(files[resume.sourceRelative]!);
    }
    begins++;
    writer = request.sourceRelative;
    files[writer!] = bytes;
    identities[writer!] =
        '${'1' * 16}:${(_identity++).toRadixString(16).padLeft(32, '0')}';
    return receipt(request, ContentTransferState.writing);
  }

  @override
  Future<ContentTransferReceipt> append(
    ContentTransferRequest request,
    Uint8List bytes,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    checkExact(request, writing: true);
    if (writer != request.sourceRelative ||
        bytes.isEmpty ||
        bytes.length > 256 * 1024 ||
        request.observedBytes + bytes.length > request.expectedSize ||
        _appendSettled != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    final settled = _appendSettled = Completer<void>();
    if (!appendEntered.isCompleted) appendEntered.complete();
    try {
      await appendGate?.future;
      cancellation.check();
      appends++;
      files[writer!] = Uint8List.fromList([...files[writer!]!, ...bytes]);
      if (!appended.isCompleted) appended.complete();
      return receipt(request, ContentTransferState.writing);
    } finally {
      _appendSettled = null;
      settled.complete();
    }
  }

  @override
  Future<ContentTransferReceipt> finish(
    ContentTransferRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    checkExact(request, writing: true);
    if (writer != request.sourceRelative ||
        request.observedBytes != request.expectedSize ||
        digest(files[request.sourceRelative]!) != request.expectedSha256) {
      throw const AppFailure(FailureKind.invalidData);
    }
    writer = null;
    if (throwAfterFinish) throw const AppFailure(FailureKind.network);
    return receipt(request, ContentTransferState.complete);
  }

  @override
  Future<ContentTransferReceipt> inspect(
    ContentTransferRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    inspections++;
    final data = files[request.sourceRelative];
    if (data != null) checkIdentity(request);
    if (data != null && request.sourceIdentity.isEmpty) {
      throw const AppFailure(FailureKind.recovery);
    }
    return receipt(
      request,
      data == null
          ? ContentTransferState.missing
          : data.length == request.expectedSize &&
                digest(data) == request.expectedSha256
          ? ContentTransferState.complete
          : ContentTransferState.checkpoint,
    );
  }

  @override
  Future<ContentTransferReceipt> discard(
    ContentTransferRequest request,
    CancellationToken cancellation,
  ) async {
    if (!files.containsKey(request.sourceRelative)) {
      cancellation.check();
      return ContentTransferReceipt(
        state: ContentTransferState.discarded,
        rootIdentity: deviceId,
        directoryIdentity: request.directoryIdentity,
        sourceIdentity: request.sourceIdentity,
        sourceRelative: request.sourceRelative,
        observedBytes: request.observedBytes,
        observedSha256: request.observedSha256,
      );
    }
    cancellation.check();
    checkExact(request);
    if (writer != null) throw const AppFailure(FailureKind.conflict);
    final observed = receipt(request, ContentTransferState.discarded);
    files.remove(request.sourceRelative);
    identities.remove(request.sourceRelative);
    discarded.add(request.sourceRelative);
    return observed;
  }

  @override
  Future<void> cancel() async {
    cancels++;
  }

  @override
  Future<void> drain() async {
    drains++;
    await _appendSettled?.future;
    await drainGate?.future;
    if (failDrain) throw const AppFailure(FailureKind.recovery);
    writer = null;
  }

  @override
  Future<void> close() async {
    await cancel();
    await drain();
  }
}

/// Handoff model reads the exact bytes and identity produced by the HTTP test
/// writer. Native content-staging tests separately verify real copy effects.
class TransferStagingBackend implements ContentStagingBackend {
  TransferStagingBackend(this.incoming);
  final TransferBackend incoming;
  bool copied = false;
  int stages = 0;
  @override
  String get deviceId => incoming.deviceId;

  ContentStagingReceipt observation(ContentStagingRequest request) {
    final data = incoming.files[request.sourceRelative];
    final identity = incoming.identities[request.sourceRelative];
    if (data == null ||
        identity == null ||
        data.length != request.size ||
        digest(data) != request.sha256 ||
        request.sourceIdentity.isNotEmpty &&
            request.sourceIdentity != identity ||
        request.rootIdentity.isNotEmpty && request.rootIdentity != deviceId) {
      throw const AppFailure(FailureKind.recovery);
    }
    return ContentStagingReceipt(
      state: copied
          ? ContentStagingState.staged
          : ContentStagingState.checkpoint,
      verifiedBytes: copied ? request.size : 0,
      verifiedChunks: copied
          ? (request.size + managedStageChunkBytes - 1) ~/
                managedStageChunkBytes
          : 0,
      rootIdentity: deviceId,
      sourceIdentity: identity,
    );
  }

  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    return observation(request);
  }

  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    observation(request);
    stages++;
    copied = true;
    return observation(request);
  }

  @override
  Future<void> cancel() async {}
  @override
  Future<void> drain() async {}
  @override
  Future<void> close() async {}
}
