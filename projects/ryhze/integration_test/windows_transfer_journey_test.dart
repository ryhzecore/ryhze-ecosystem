// Real HTTP, Flutter channels, native files, production services and app widgets.
// The dedicated native test runner supplies an exclusive temporary root. This
// entrypoint refuses the production runner before opening a journal or server.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:ryhze/app.dart';
import 'package:ryhze/app/app_services.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/data/journal_store.dart';
import 'package:ryhze/design/controls.dart';
import 'package:ryhze/design/tokens.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/installations/content_staging_backend.dart';
import 'package:ryhze/features/installations/managed_content_admission.dart';
import 'package:ryhze/features/installations/managed_content_manifest.dart';
import 'package:ryhze/features/installations/managed_installation.dart';
import 'package:ryhze/features/installations/managed_transfer_record.dart';
import 'package:ryhze/features/installations/managed_transfer_source.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/features/store/catalog_repository.dart';
import 'package:ryhze/platform/windows/windows_content_staging.dart';
import 'package:ryhze/platform/windows/windows_content_transfer.dart';
import 'package:ryhze/providers/managed_content_transport.dart';
import 'package:ryhze/providers/managed_content_transport_io.dart';

import '../test_driver/journey_deadline.dart';
import '../test_driver/transfer_journey_result.dart';
import 'windows_process_probe.dart';

String _digest(List<int> bytes) => crypto.sha256.convert(bytes).toString();

// Deliberately synthetic authority, linked only from this test entrypoint. It
// exercises the trusted adapter's positive assertion without claiming real
// sign-in, content ownership or publisher approval. The bytes are inert.
class _JourneyAuthority implements ManagedContentTransferAuthority {
  _JourneyAuthority(this.bytes, this.transport);
  final Uint8List bytes;
  final ManagedContentTransport transport;
  final _listeners = <void Function()>[];
  ManagedManifestProvenance? _provenance;
  bool deny = false;
  int authorizations = 0;
  @override
  bool get available => true;
  @override
  String get authority => 'test-only-journey-authority';
  @override
  String get policy => 'test-only-inert-content';
  @override
  int generation = 1;
  @override
  void addInvalidationListener(void Function() value) => _listeners.add(value);
  @override
  void removeInvalidationListener(void Function() value) =>
      _listeners.remove(value);
  void changeApproval(bool denied) {
    deny = denied;
    generation++;
    for (final listener in List.of(_listeners)) {
      listener();
    }
  }

  @override
  Future<ManagedContentGrant> authorize(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    authorizations++;
    if (deny) throw const AppFailure(FailureKind.denied);
    final now = DateTime.now().toUtc();
    _provenance = ManagedManifestProvenance.create(
      authority: authority,
      policy: policy,
      sourceManifestSha256: _digest(bytes),
      normalizedBytes: bytes,
      observedAt: now,
      expiresAt: now.add(const Duration(minutes: 4)),
    );
    return ManagedContentGrant(
      scope: request.scope,
      sessionGeneration: request.sessionGeneration,
      deviceId: request.deviceId,
      provenance: _provenance!,
      state: AccessState.verified,
      fixture: false,
    );
  }

  @override
  Future<ManagedTransferSource> resolveTransfer(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (deny) throw const AppFailure(FailureKind.denied);
    return ManagedTransferSource(
      request: request,
      provenance: _provenance!,
      sourceKey: 'test-only-loopback-content-v1',
      transport: transport,
      fixture: false,
    );
  }
}

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('Isolated Windows transfer cancellation, retry and staging', (
    tester,
  ) async {
    expect(Platform.isWindows, isTrue);
    final report = <String, Object?>{
      'source': const String.fromEnvironment('RYHZE_SOURCE'),
      'workload': 'windows-native-transfer-loopback-v2',
      'acceptance_revision': 2,
      'status': 'running',
      'cleanup_completed': false,
      'completed_phases': <String>[],
      'limits': transferJourneyLimits,
    };
    binding.reportData = {'transfer_journey': report};
    final deadline = JourneyBodyDeadline(budget: const Duration(seconds: 160));
    final elapsed = Stopwatch()..start();
    final timings = <String, int>{};
    var previousMicros = 0;
    var cleanupStarted = false,
        bodyCompleted = false,
        bodyFailed = false,
        operationTimedOut = false;
    AppServices? app;
    JournalStore? openedStore;
    WindowsContentTransfer? transfer;
    WindowsContentStaging? staging;
    HttpServer? server;
    final sockets = <Socket>[];
    final requests = <String?>[];
    final responders = <Future<void>>[];
    final requestEvidence = <Map<String, Object?>>[];
    final responderFailures = <Map<String, Object?>>[];
    final cleanupFailures = <Map<String, Object?>>[];
    (Object, StackTrace)? firstResponderFailure;
    Directory? journal;

    Future<T> bounded<T>(Future<T> Function() work, String label) async {
      report['pending_operation'] = label;
      try {
        final result = cleanupStarted
            ? await Future<T>.sync(work).timeout(const Duration(seconds: 15))
            : await deadline.run(work, timeout: const Duration(seconds: 25));
        report.remove('pending_operation');
        return result;
      } catch (error) {
        if (error is TimeoutException) {
          operationTimedOut = true;
        }
        report.putIfAbsent('failed_operation', () => label);
        rethrow;
      }
    }

    Future<void> pump() => bounded(
      () => tester.pump(const Duration(milliseconds: 80)),
      'render frame',
    );
    Future<void> until(bool Function() ready, String label) async {
      try {
        final wait = Stopwatch()..start();
        while (!ready() && wait.elapsed < const Duration(seconds: 20)) {
          await pump();
        }
        expect(ready(), isTrue, reason: label);
        await pump();
      } catch (_) {
        report.putIfAbsent('failed_operation', () => label);
        rethrow;
      }
    }

    Finder control(String label) => find.byWidgetPredicate(
      (widget) => widget is RyhzeControl && widget.label == label,
    );
    Future<void> tap(String label) async {
      // Production review routes remain animated; wait for the existing design
      // duration before calculating the next control's screen position.
      await bounded(
        () => tester.pump(RyhzeTokens.frameIn),
        'settle route before $label',
      );
      final target = control(label);
      expect(target, findsOneWidget);
      expect(tester.widget<RyhzeControl>(target).onPressed, isNotNull);
      await bounded(() => tester.ensureVisible(target), 'reveal $label');
      await bounded(() => tester.tap(target), 'tap $label');
      await pump();
    }

    void done(String phase) {
      final micros = elapsed.elapsedMicroseconds;
      timings[phase] = micros - previousMicros;
      previousMicros = micros;
      (report['completed_phases'] as List<String>).add(phase);
    }

    try {
      final isolation = await bounded(
        () =>
            const MethodChannel('com.ryhze.test/transfer-isolation.v1')
                .invokeMapMethod<String, Object?>('receipt'),
        'read exclusive native root receipt',
      );
      expect(isolation, isNotNull);
      expect(isolation!.length, 7);
      expect(isolation['schemaVersion'], 1);
      expect(isolation['fixture'], 'exclusive-native-transfer-journey-v1');
      expect(isolation['processId'], pid);
      final nonce = isolation['nonce'] as String;
      expect(RegExp(r'^[0-9a-f]{64}$').hasMatch(nonce), isTrue);
      final root = isolation['rootDirectory'] as String;
      expect(root.endsWith('\\ryhze-transfer-journey-$nonce'), isTrue);
      expect(
        ContentStagingRequest.validIdentity(
          isolation['rootIdentity'] as String,
        ),
        isTrue,
      );
      expect(
        await bounded(
          () => File('$root/isolation.txt').readAsString(),
          'verify native ownership marker',
        ),
        nonce,
      );
      journal = Directory('$root/journal');
      expect(
        await bounded(() => journal!.list().isEmpty, 'verify new journal'),
        isTrue,
      );
      transfer = await bounded(
        WindowsContentTransfer.connect,
        'connect real native transfer',
      );
      staging = await bounded(
        WindowsContentStaging.connect,
        'connect real native staging',
      );
      expect(transfer, isNotNull);
      expect(staging, isNotNull);
      expect(transfer!.deviceId, isolation['deviceId']);
      expect(staging!.deviceId, transfer.deviceId);
      expect(staging.discardAvailable, isTrue);
      expect(await Directory('$root/incoming').list().isEmpty, isTrue);
      expect(await Directory('$root/content-staging').list().isEmpty, isTrue);
      report.addAll({
        'isolation_verified': true,
        'isolation_nonce': nonce,
        'isolation_root_identity': isolation['rootIdentity'],
        'device_id': transfer.deviceId,
        'native_transfer_available': true,
        'native_staging_available': true,
        'native_discard_available': true,
      });
      done(transferJourneyPhases[0]);

      const prefix = 65536, size = 600000;
      final payload = Uint8List.fromList(List.generate(size, (i) => i % 251));
      final operation = _digest(utf8.encode('inert-transfer-$nonce'));
      final manifest = Uint8List.fromList(
        utf8.encode(
          jsonEncode({
            'version': 1,
            'provider': 'test-journey',
            'item': 'inert-payload',
            'namespace': 'exclusive-test-root',
            'packageId': 'journey-inert-content',
            'packageVersion': '1',
            'payloadSize': size,
            'payloadSha256': _digest(payload),
            'files': [
              {
                'path': 'payload.bin',
                'size': size,
                'sha256': _digest(payload),
                'executable': false,
              },
            ],
          }),
        ),
      );
      server = await bounded(
        () => HttpServer.bind(InternetAddress.loopbackIPv4, 0),
        'bind loopback fixture',
      );
      Future<void> respond(HttpRequest request) async {
        unawaited(
          request.response.done.then<void>((_) {}, onError: (Object _) {}),
        );
        try {
          final range = request.headers.value(HttpHeaders.rangeHeader);
          requests.add(range);
          requestEvidence.add({
            'sequence': requests.length,
            'method': request.method,
            'path': request.uri.path,
            'range': range,
            'if_range': request.headers.value(HttpHeaders.ifRangeHeader),
          });
          report['http_requests'] = requestEvidence;
          // This HTTP callback can run during tester.pump; expectSync keeps
          // matcher failures without conflicting with that guarded future.
          expectSync(request.method, 'GET');
          expectSync(request.uri.path, '/inert-content');
          if (requests.length == 1) {
            expectSync(range, isNull);
            final socket = await request.response.detachSocket(
              writeHeaders: false,
            );
            sockets.add(socket);
            socket.write(
              'HTTP/1.1 200 OK\r\nContent-Length: $size\r\nETag: "journey-v1"\r\nConnection: close\r\n\r\n',
            );
            socket.add(payload.sublist(0, prefix));
            await socket.flush();
          } else {
            expectSync(requests.length, 2);
            expectSync(range, 'bytes=$prefix-${size - 1}');
            expectSync(
              request.headers.value(HttpHeaders.ifRangeHeader),
              '"journey-v1"',
            );
            request.response
              ..statusCode = 206
              ..contentLength = size - prefix;
            request.response.headers
              ..set(HttpHeaders.etagHeader, '"journey-v1"')
              ..set(
                HttpHeaders.contentRangeHeader,
                'bytes $prefix-${size - 1}/$size',
              );
            request.response.add(payload.sublist(prefix));
            await request.response.close();
          }
        } on SocketException {
          // The first connection is deliberately cancelled by the UI.
        } on HttpException {
          // Cancellation may surface through the HTTP response owner.
        } catch (error, stack) {
          firstResponderFailure ??= (error, stack);
          responderFailures.add({
            'operation': 'loopback response ${requests.length}',
            'type': error.runtimeType.toString(),
            'error': error.toString(),
            'stack': stack.toString(),
          });
          report['responder_failures'] = responderFailures;
          rethrow;
        }
      }

      server!.listen((request) {
        final response = respond(request);
        responders.add(response);
        unawaited(response.catchError((Object _) {}));
      });
      final authority = _JourneyAuthority(
        manifest,
        ManagedContentHttpTransport.loopbackForTesting(
          Uri.parse('http://127.0.0.1:${server.port}/inert-content'),
        ),
      );
      openedStore = await bounded(
        () => JournalStore.open(journal!),
        'open isolated durable journal',
      );
      final owner = app = AppServices(
        storage: openedStore!,
        catalogRepository: LocalCatalogRepository(),
        contentTransfer: transfer,
        contentStaging: staging,
        transferAuthority: authority,
      );
      const scope = AccountScope.verified(
        'test-only-journey',
        'synthetic-owner',
      );
      owner.session.replace(scope);
      final controller = owner.transferController!;
      final intent = ManagedStageIntent(
        operationId: operation,
        item: ProviderItemRef(
          'test-journey',
          'inert-payload',
          namespace: 'exclusive-test-root',
        ),
        packageId: 'journey-inert-content',
        version: '1',
        manifestSha256: _digest(manifest),
        sourceRelative: 'transfer-0/$operation-0.bin',
        expectedSize: size,
        expectedSha256: _digest(payload),
        slot: 0,
      );
      final countersBefore = processCounters();
      final rssBefore = ProcessInfo.currentRss;
      await bounded(
        () => tester.pumpWidget(RyhzeApp(services: owner)),
        'mount real application',
      );
      final initial = controller.download(intent);
      await until(() {
        final observation = <String, Object?>{
          'received_bytes': owner.managedTransfer!.receivedBytes,
          'controller_busy': controller.busy,
          'controller_failure': controller.failure?.kind.name,
          'transfer_state': owner.transfers!.findCurrent(operation)?.state.name,
          'network_requests': requests.length,
        };
        report['prefix_observation'] = observation;
        final responseFailure = firstResponderFailure;
        if (responseFailure != null) {
          Error.throwWithStackTrace(responseFailure.$1, responseFailure.$2);
        }
        if (!controller.busy &&
            owner.managedTransfer!.receivedBytes != prefix) {
          throw TestFailure(
            'Transfer ended before actual native prefix: '
            '${jsonEncode(observation)}; requests: ${jsonEncode(requestEvidence)}',
          );
        }
        return owner.managedTransfer!.receivedBytes == prefix;
      }, 'actual native prefix');
      expect(
        owner.transfers!.findCurrent(operation)!.request.sourceIdentity,
        isNotEmpty,
      );
      await bounded(
        () => tester.tap(find.byKey(const Key('menu-trigger'))),
        'open menu',
      );
      await bounded(() => tester.pump(RyhzeTokens.frameIn), 'settle menu');
      await bounded(
        () => tester.tap(find.byKey(const Key('menu-workspace'))),
        'open workspace',
      );
      await pump();
      await tap('Activity');
      done(transferJourneyPhases[1]);

      await tap('Cancel download work');
      await bounded(() => initial, 'settle actual cancelled transfer');
      await until(() => !controller.busy, 'cancelled worker drain');
      await tap('Inspect download journey-inert-content');
      await until(() => !controller.busy, 'inspect retained prefix');
      var record = owner.transfers!.findCurrent(operation)!;
      expect(record.state, ManagedTransferState.recoveryRequired);
      expect(record.request.observedBytes, prefix);
      expect(
        record.request.observedSha256,
        _digest(payload.sublist(0, prefix)),
      );
      final firstPath = '$root/incoming/${record.request.sourceRelative}';
      expect(await File(firstPath).length(), prefix);
      done(transferJourneyPhases[2]);

      authority.changeApproval(true);
      final authorizations = authority.authorizations;
      await tap('Retry download journey-inert-content');
      await until(() => !controller.busy, 'reject revoked authority');
      expect(controller.failure?.kind, FailureKind.denied);
      expect(authority.authorizations, greaterThan(authorizations));
      expect(requests.length, 1);
      expect(owner.transfers!.findCurrent(operation)!.attempt, 0);
      report['revalidation_refusal_verified'] = true;
      done(transferJourneyPhases[3]);

      authority.changeApproval(false);
      final retryWatch = Stopwatch()..start();
      await tap('Retry download journey-inert-content');
      await until(() => !controller.busy, 'complete real Range retry');
      retryWatch.stop();
      expect(controller.failure, isNull);
      record = owner.transfers!.findCurrent(operation)!;
      expect(record.state, ManagedTransferState.downloaded);
      expect(record.request.observedBytes, size);
      expect(record.request.observedSha256, _digest(payload));
      expect(record.previous, isNotNull);
      final completedPath = '$root/incoming/${record.request.sourceRelative}';
      expect(await File(completedPath).length(), size);
      expect(
        _digest(await File(completedPath).readAsBytes()),
        _digest(payload),
      );
      expect(requests, [null, 'bytes=$prefix-${size - 1}']);
      report['retry_ui_pipeline_micros'] = retryWatch.elapsedMicroseconds;
      done(transferJourneyPhases[4]);

      for (var i = 0; i < 2; i++) {
        await tap('Inspect download journey-inert-content');
        await until(() => !controller.busy, 'repeat read-only inspection');
        expect(controller.failure, isNull);
      }
      expect(requests.length, 2);
      done(transferJourneyPhases[5]);

      expect(await Directory('$root/content-staging/slot-0').exists(), isFalse);
      await tap('Stage download journey-inert-content');
      await until(() => !controller.busy, 'complete native staging');
      expect(controller.failure, isNull);
      expect(
        owner.managedInstallations!.findCurrent(operation)!.state,
        ManagedInstallationState.staged,
      );
      expect(
        owner.transfers!.findCurrent(operation)!.state,
        ManagedTransferState.stagingAdmitted,
      );
      expect(await Directory('$root/content-staging/slot-0').exists(), isTrue);
      await tap('Installations');
      done(transferJourneyPhases[6]);

      await tap('Review removal for journey-inert-content');
      await until(
        () => control('Remove staging copy').evaluate().isNotEmpty,
        'staging removal review',
      );
      await tap('Cancel removal');
      await until(
        () => control('Remove staging copy').evaluate().isEmpty,
        'cancel staging review',
      );
      expect(await Directory('$root/content-staging/slot-0').exists(), isTrue);
      await tap('Review removal for journey-inert-content');
      await until(
        () => control('Remove staging copy').evaluate().isNotEmpty,
        'second staging review',
      );
      await tap('Remove staging copy');
      await until(
        () =>
            !owner.managedContentController!.busy &&
            owner.managedInstallations!.findCurrent(operation)!.state ==
                ManagedInstallationState.discarded,
        'reviewed staging removal',
      );
      expect(await Directory('$root/content-staging/slot-0').exists(), isFalse);
      expect(await File(completedPath).length(), size);
      report['staging_absence_verified'] = true;
      done(transferJourneyPhases[7]);

      await tap('Review removal of download journey-inert-content');
      await until(
        () => !controller.busy && controller.removalId != null,
        'incoming removal review',
      );
      await tap('Keep downloaded files');
      expect(await File(completedPath).exists(), isTrue);
      await tap('Review removal of download journey-inert-content');
      await until(
        () => !controller.busy && controller.removalId != null,
        'second incoming review',
      );
      await tap('Confirm removal of journey-inert-content');
      await until(() => !controller.busy, 'remove reviewed incoming attempts');
      expect(controller.failure, isNull);
      expect(
        owner.transfers!.findCurrent(operation)!.state,
        ManagedTransferState.discarded,
      );
      expect(await File(firstPath).exists(), isFalse);
      expect(await File(completedPath).exists(), isFalse);
      report['incoming_absence_verified'] = true;
      report['owned_files_removed'] = true;
      done(transferJourneyPhases[8]);

      report.addAll({
        'operation_id': operation,
        'payload_sha256': _digest(payload),
        'slot': 0,
        'prefix_bytes': prefix,
        'payload_bytes': size,
        'network_requests': requests.length,
        'resume_range': requests.last,
        'authority_checks': authority.authorizations,
        'rss_before': rssBefore,
        'rss_after': ProcessInfo.currentRss,
        'process_counters_before': countersBefore,
        'process_counters_after': processCounters(),
      });
      final persisted = jsonEncode(owner.storage.read());
      await bounded(
        () => tester.pumpWidget(const SizedBox.shrink()),
        'unmount application',
      );
      await bounded(owner.close, 'close and drain actual owners');
      app = null;
      openedStore = null;
      transfer = null;
      staging = null;
      final reopened = await bounded(
        () => JournalStore.open(journal!),
        'reopen isolated journal',
      );
      try {
        expect(jsonEncode(reopened.read()), persisted);
        report['journal_reopened'] = true;
      } finally {
        await bounded(reopened.close, 'close reopened journal');
      }
      done(transferJourneyPhases[9]);
      bodyCompleted = true;
    } catch (error, stack) {
      bodyFailed = true;
      report['body_failure'] = {
        'operation': report['failed_operation'] ?? report['pending_operation'],
        'type': error.runtimeType.toString(),
        'error': error.toString(),
        'stack': stack.toString(),
      };
      rethrow;
    } finally {
      cleanupStarted = true;
      deadline.stop();
      // A timeout stops waiting, not the underlying setup or native future.
      // Even if visible owners close, do not claim all work settled afterward.
      var cleanupOkay = !operationTimedOut;
      Future<void> settle(Future<void> Function() action, String name) async {
        try {
          await bounded(action, name);
        } catch (error, stack) {
          cleanupOkay = false;
          cleanupFailures.add({
            'operation': name,
            'type': error.runtimeType.toString(),
            'error': error.toString(),
            'stack': stack.toString(),
          });
          report['cleanup_failures'] = cleanupFailures;
        }
      }

      await settle(
        () => tester.pumpWidget(const SizedBox.shrink()),
        'dispose remaining widgets',
      );
      if (app != null) {
        await settle(app.close, 'close remaining owners');
      } else {
        if (transfer != null) {
          await settle(transfer.close, 'close unowned transfer');
        }
        if (staging != null) {
          await settle(staging.close, 'close unowned staging');
        }
        if (openedStore != null) {
          await settle(openedStore.close, 'close unowned journal');
        }
      }
      for (final socket in sockets) {
        socket.destroy();
      }
      if (server != null) {
        await settle(() async {
          await server!.close(force: true);
        }, 'close loopback server');
      }
      await settle(() async {
        await Future.wait(responders);
      }, 'settle response assertions');
      report['phase_micros'] = timings;
      report['operation_timed_out'] = operationTimedOut;
      report['cleanup_completed'] = cleanupOkay;
      report['status'] = bodyCompleted && cleanupOkay ? 'completed' : 'failed';
      // Teardown evidence remains decisive for success, but cannot replace the
      // body error and its original stack when the journey already failed.
      if (!bodyFailed) {
        expect(
          cleanupOkay,
          isTrue,
          reason: 'Actual owners must settle. Fixture retained on failure.',
        );
      }
    }
  }, timeout: const Timeout(Duration(seconds: 230)));
}
