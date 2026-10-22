import 'dart:async';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../features/installations/content_staging_backend.dart';
import '../../features/installations/content_transfer_backend.dart';
import '../../features/installations/managed_installation.dart';

class WindowsContentTransfer implements ContentTransferBackend {
  WindowsContentTransfer._(this._channel, this.deviceId);
  final MethodChannel _channel;
  @override
  final String deviceId;
  bool _closed = false, _running = false;
  Future<void>? _draining, _closing;

  static Future<WindowsContentTransfer?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.content-transfer.v1',
    ),
  }) async {
    try {
      final value = await channel
          .invokeMethod<Object?>('probe')
          .timeout(const Duration(seconds: 2));
      if (value is! Map ||
          value.length != 5 ||
          value['schemaVersion'] is! int ||
          value['schemaVersion'] != 1 ||
          value['transfer'] != true ||
          value['activation'] != false ||
          value['authenticity'] != false ||
          value['deviceId'] is! String ||
          (value['deviceId'] as String).isEmpty ||
          !ContentStagingRequest.validIdentity(value['deviceId'] as String)) {
        return null;
      }
      return WindowsContentTransfer._(channel, value['deviceId'] as String);
    } catch (_) {
      return null;
    }
  }

  static Map<String, Object> _descriptor(ContentTransferRequest r) => {
    'slot': r.slot,
    'attempt': r.attempt,
    'operationId': r.operationId,
    'expectedSize': r.expectedSize,
    'expectedSha256': Uint8List.fromList([
      for (var i = 0; i < r.expectedSha256.length; i += 2)
        int.parse(r.expectedSha256.substring(i, i + 2), radix: 16),
    ]),
    'rootIdentity': r.rootIdentity,
    'directoryIdentity': r.directoryIdentity,
    'sourceIdentity': r.sourceIdentity,
    'observedBytes': r.observedBytes,
    'observedSha256': r.observedSha256,
  };

  Future<ContentTransferReceipt> _run(
    String method,
    ContentTransferRequest request,
    CancellationToken cancellation, {
    ContentTransferRequest? resume,
    Uint8List? bytes,
  }) async {
    cancellation.check();
    if (_closed) throw const AppFailure(FailureKind.unsupported);
    if (_running || _draining != null) {
      throw const AppFailure(FailureKind.conflict);
    }
    if ((request.rootIdentity.isNotEmpty && request.rootIdentity != deviceId) ||
        ((method == 'append' || method == 'finish') &&
            (request.rootIdentity != deviceId ||
                request.directoryIdentity.isEmpty ||
                request.sourceIdentity.isEmpty ||
                request.observedSha256.isNotEmpty)) ||
        (method == 'begin' &&
            (request.sourceIdentity.isNotEmpty ||
                request.observedBytes != 0 ||
                request.observedSha256.isNotEmpty)) ||
        (method == 'append' &&
            (bytes == null ||
                bytes.isEmpty ||
                bytes.length > managedStageChunkBytes ||
                bytes.length > request.expectedSize - request.observedBytes)) ||
        (method == 'discard' &&
            (request.rootIdentity != deviceId ||
                (request.sourceIdentity.isEmpty
                    ? (request.observedBytes != 0 ||
                          request.observedSha256.isNotEmpty)
                    : request.directoryIdentity.isEmpty)))) {
      throw const AppFailure(FailureKind.invalidData);
    }
    if (resume != null &&
        (method != 'begin' ||
            resume.slot != request.slot ||
            resume.attempt == request.attempt ||
            resume.operationId != request.operationId ||
            resume.expectedSize != request.expectedSize ||
            resume.expectedSha256 != request.expectedSha256 ||
            resume.rootIdentity != deviceId ||
            request.rootIdentity != deviceId ||
            resume.directoryIdentity.isEmpty ||
            resume.directoryIdentity != request.directoryIdentity ||
            resume.sourceIdentity.isEmpty ||
            resume.observedSha256.isEmpty ||
            resume.observedBytes >= request.expectedSize)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    _running = true;
    final unlink = cancellation.onCancel(() {
      unawaited(cancel().catchError((Object _) {}));
    });
    try {
      final value = await _channel.invokeMethod<Object?>(method, {
        'request': _descriptor(request),
        if (method == 'begin')
          'resume': resume == null ? null : _descriptor(resume),
        if (bytes != null) 'bytes': Uint8List.fromList(bytes),
      });
      cancellation.check();
      if (value is! Map ||
          value.length != 7 ||
          value['state'] is! String ||
          value['rootIdentity'] is! String ||
          value['directoryIdentity'] is! String ||
          value['sourceIdentity'] is! String ||
          value['sourceRelative'] is! String ||
          value['observedBytes'] is! int ||
          value['observedSha256'] is! String) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final state = ContentTransferState.values
          .where((s) => s.name == value['state'])
          .firstOrNull;
      final root = value['rootIdentity'] as String,
          directory = value['directoryIdentity'] as String,
          source = value['sourceIdentity'] as String,
          digest = value['observedSha256'] as String;
      final count = value['observedBytes'] as int;
      final observed =
          state == ContentTransferState.complete ||
          state == ContentTransferState.checkpoint;
      final allowed = switch (method) {
        'begin' || 'append' => {ContentTransferState.writing},
        'finish' => {ContentTransferState.complete},
        'inspect' => {
          ContentTransferState.checkpoint,
          ContentTransferState.complete,
          ContentTransferState.missing,
        },
        'discard' => {ContentTransferState.discarded},
        _ => <ContentTransferState>{},
      };
      if (state == null ||
          !allowed.contains(state) ||
          root != deviceId ||
          !ContentStagingRequest.validIdentity(directory) ||
          !ContentStagingRequest.validIdentity(source) ||
          (request.directoryIdentity.isNotEmpty &&
              directory != request.directoryIdentity &&
              !(state == ContentTransferState.missing && directory.isEmpty)) ||
          value['sourceRelative'] != request.sourceRelative ||
          count < 0 ||
          count > request.expectedSize ||
          (digest.isNotEmpty &&
              !ContentStagingRequest.digest.hasMatch(digest)) ||
          (state != ContentTransferState.missing &&
              state != ContentTransferState.discarded &&
              directory.isEmpty) ||
          (observed &&
              (source.isEmpty ||
                  digest.isEmpty ||
                  request.sourceIdentity.isEmpty ||
                  request.directoryIdentity.isEmpty)) ||
          (observed &&
              request.sourceIdentity.isNotEmpty &&
              source != request.sourceIdentity) ||
          (state == ContentTransferState.complete &&
              (count != request.expectedSize ||
                  digest != request.expectedSha256)) ||
          (state == ContentTransferState.writing &&
              (source.isEmpty || digest.isNotEmpty)) ||
          (method == 'append' && source != request.sourceIdentity) ||
          (method == 'begin' && count != (resume?.observedBytes ?? 0)) ||
          (method == 'append' &&
              count != request.observedBytes + bytes!.length) ||
          (state == ContentTransferState.missing &&
              (source.isNotEmpty || digest.isNotEmpty || count != 0)) ||
          (state == ContentTransferState.discarded &&
              (source != request.sourceIdentity ||
                  count != request.observedBytes ||
                  digest != request.observedSha256))) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return ContentTransferReceipt(
        state: state,
        rootIdentity: root,
        directoryIdentity: directory,
        sourceIdentity: source,
        sourceRelative: value['sourceRelative'] as String,
        observedBytes: count,
        observedSha256: digest,
      );
    } on PlatformException catch (error) {
      throw AppFailure(switch (error.code) {
        'cancelled' => FailureKind.cancelled,
        'conflict' => FailureKind.conflict,
        'invalidData' => FailureKind.invalidData,
        'storage' => FailureKind.storage,
        'recovery' => FailureKind.recovery,
        _ => FailureKind.unsupported,
      });
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    } finally {
      unlink();
      _running = false;
    }
  }

  @override
  Future<ContentTransferReceipt> begin(
    ContentTransferRequest request,
    CancellationToken cancellation, {
    ContentTransferRequest? resume,
  }) => _run('begin', request, cancellation, resume: resume);
  @override
  Future<ContentTransferReceipt> append(
    ContentTransferRequest request,
    Uint8List bytes,
    CancellationToken cancellation,
  ) => _run('append', request, cancellation, bytes: bytes);
  @override
  Future<ContentTransferReceipt> finish(
    ContentTransferRequest request,
    CancellationToken cancellation,
  ) => _run('finish', request, cancellation);
  @override
  Future<ContentTransferReceipt> inspect(
    ContentTransferRequest request,
    CancellationToken cancellation,
  ) => _run('inspect', request, cancellation);
  @override
  Future<ContentTransferReceipt> discard(
    ContentTransferRequest request,
    CancellationToken cancellation,
  ) => _run('discard', request, cancellation);
  @override
  Future<void> cancel() => _channel.invokeMethod<void>('cancel');
  @override
  Future<void> drain() =>
      _draining ??= _channel.invokeMethod<void>('drain').whenComplete(() {
        _draining = null;
      });
  @override
  Future<void> close() {
    _closed = true;
    return _closing ??= _channel.invokeMethod<void>('close');
  }
}
