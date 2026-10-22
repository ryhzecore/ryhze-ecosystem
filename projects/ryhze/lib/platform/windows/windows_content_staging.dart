import 'dart:async';

import 'package:flutter/services.dart';

import '../../core/app_failure.dart';
import '../../features/installations/content_staging_backend.dart';
import '../../features/installations/content_discard_backend.dart';
import '../../features/installations/managed_installation.dart';

class WindowsContentStaging
    implements ContentStagingBackend, ContentDiscardBackend {
  WindowsContentStaging._(this._channel, this.deviceId, this.discardAvailable);
  @override
  final String deviceId;
  @override
  final bool discardAvailable;
  final MethodChannel _channel;
  bool _closed = false, _running = false;
  Future<void>? _closing;
  Future<void>? _draining;
  static Future<WindowsContentStaging?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.content-staging.v1',
    ),
  }) async {
    try {
      final value = await channel
          .invokeMethod<Object?>('probe')
          .timeout(const Duration(seconds: 2));
      if (value is! Map ||
          value.length != 5 ||
          value['schemaVersion'] != 1 ||
          value['schemaVersion'] is! int ||
          value['staging'] != true ||
          value['activation'] != false ||
          value['authenticity'] != false ||
          value['deviceId'] is! String ||
          (value['deviceId'] as String).isEmpty ||
          !ContentStagingRequest.validIdentity(value['deviceId'] as String)) {
        return null;
      }
      bool discardAvailable = false;
      try {
        final discard = await channel
            .invokeMethod<Object?>('probeDiscard')
            .timeout(const Duration(seconds: 2));
        discardAvailable =
            discard is Map &&
            discard.length == 2 &&
            discard['schemaVersion'] is int &&
            discard['schemaVersion'] == 1 &&
            discard['discard'] == true;
      } catch (_) {
        // An older host's optional capability failure keeps staging available.
      }
      return WindowsContentStaging._(
        channel,
        value['deviceId'] as String,
        discardAvailable,
      );
    } catch (_) {
      return null;
    }
  }

  static Uint8List _bytes(String hex) => Uint8List.fromList([
    for (var i = 0; i < hex.length; i += 2)
      int.parse(hex.substring(i, i + 2), radix: 16),
  ]);
  static Map<String, Object> _descriptor(ContentStagingRequest request) => {
    'slot': request.slot,
    'operationId': _bytes(request.operationId),
    'manifestSha256': _bytes(request.manifestSha256),
    'sourceRelative': request.sourceRelative,
    'size': request.size,
    'minimumVerifiedBytes': request.minimumVerifiedBytes,
    'sha256': _bytes(request.sha256),
    'rootIdentity': request.rootIdentity,
    'sourceIdentity': request.sourceIdentity,
  };
  Future<ContentStagingReceipt> _run(
    String method,
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (_closed) throw const AppFailure(FailureKind.unsupported);
    if (_running) throw const AppFailure(FailureKind.conflict);
    if (method == 'stage' &&
        (request.rootIdentity.isEmpty || request.sourceIdentity.isEmpty)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    _running = true;
    final remove = cancellation.onCancel(() {
      unawaited(cancel().catchError((Object _) {}));
    });
    try {
      final value = await _channel.invokeMethod<Object?>(
        method,
        _descriptor(request),
      );
      cancellation.check();
      if (value is! Map ||
          value.length != 5 ||
          value['state'] is! String ||
          value['verifiedBytes'] is! int ||
          value['verifiedChunks'] is! int ||
          value['rootIdentity'] is! String ||
          value['sourceIdentity'] is! String) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final state = ContentStagingState.values
          .where((s) => s.name == value['state'])
          .firstOrNull;
      final bytes = value['verifiedBytes'] as int,
          chunks = value['verifiedChunks'] as int;
      final root = value['rootIdentity'] as String,
          source = value['sourceIdentity'] as String;
      if (state == null ||
          bytes < 0 ||
          bytes > request.size ||
          chunks < 0 ||
          chunks >
              (request.size + managedStageChunkBytes - 1) ~/
                  managedStageChunkBytes ||
          bytes != (chunks * managedStageChunkBytes).clamp(0, request.size) ||
          !ContentStagingRequest.validIdentity(root) ||
          !ContentStagingRequest.validIdentity(source) ||
          ((state == ContentStagingState.staged ||
                  state == ContentStagingState.checkpoint) &&
              (root != deviceId || source.isEmpty)) ||
          (state == ContentStagingState.staged &&
              (bytes != request.size ||
                  chunks !=
                      (request.size + managedStageChunkBytes - 1) ~/
                          managedStageChunkBytes ||
                  root.isEmpty ||
                  source.isEmpty)) ||
          (state == ContentStagingState.staged &&
              ((request.rootIdentity.isNotEmpty &&
                      root != request.rootIdentity) ||
                  (request.sourceIdentity.isNotEmpty &&
                      source != request.sourceIdentity)))) {
        throw const AppFailure(FailureKind.invalidData);
      }
      return ContentStagingReceipt(
        state: state,
        verifiedBytes: bytes,
        verifiedChunks: chunks,
        rootIdentity: root,
        sourceIdentity: source,
      );
    } on PlatformException catch (error) {
      throw AppFailure(switch (error.code) {
        'cancelled' => FailureKind.cancelled,
        'conflict' => FailureKind.conflict,
        'invalidData' => FailureKind.invalidData,
        'recovery' => FailureKind.recovery,
        _ => FailureKind.unsupported,
      });
    } on MissingPluginException {
      throw const AppFailure(FailureKind.unsupported);
    } finally {
      remove();
      _running = false;
    }
  }

  @override
  Future<ContentStagingReceipt> stage(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) => _run('stage', request, cancellation);
  @override
  Future<ContentStagingReceipt> inspect(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) => _run('inspect', request, cancellation);

  Future<Object?> _discardCall(
    String method,
    Map<String, Object> arguments,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    if (_closed || !discardAvailable) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (_running) throw const AppFailure(FailureKind.conflict);
    _running = true;
    final unlink = cancellation.onCancel(() {
      unawaited(cancel().catchError((Object _) {}));
    });
    try {
      final result = await _channel.invokeMethod<Object?>(method, arguments);
      cancellation.check();
      return result;
    } on PlatformException catch (error) {
      throw AppFailure(switch (error.code) {
        'cancelled' => FailureKind.cancelled,
        'conflict' => FailureKind.conflict,
        'invalidData' => FailureKind.invalidData,
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
  Future<ContentDiscardPlanReceipt> planDiscard(
    ContentStagingRequest request,
    CancellationToken cancellation,
  ) async {
    if (request.rootIdentity != deviceId || request.sourceIdentity.isEmpty) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final value = await _discardCall(
      'planDiscard',
      _descriptor(request),
      cancellation,
    );
    if (value is! Map ||
        value.length != 4 ||
        value['state'] is! String ||
        value['plan'] is! Uint8List ||
        value['verifiedBytes'] is! int ||
        value['verifiedChunks'] is! int) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final state = ContentStagingState.values
        .where((s) => s.name == value['state'])
        .firstOrNull;
    final plan = value['plan'] as Uint8List;
    final bytes = value['verifiedBytes'] as int,
        chunks = value['verifiedChunks'] as int;
    final success =
        state == ContentStagingState.staged ||
        state == ContentStagingState.checkpoint;
    if (state == null ||
        plan.length > contentDiscardMaximumPlanBytes ||
        bytes < 0 ||
        bytes > request.size ||
        chunks < 0 ||
        chunks >
            (request.size + managedStageChunkBytes - 1) ~/
                managedStageChunkBytes ||
        bytes != (chunks * managedStageChunkBytes).clamp(0, request.size) ||
        (success && (plan.isEmpty || bytes < request.minimumVerifiedBytes)) ||
        (!success && plan.isNotEmpty) ||
        (state == ContentStagingState.staged && bytes != request.size)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return ContentDiscardPlanReceipt(
      state: state,
      plan: plan.isEmpty ? null : plan,
      verifiedBytes: bytes,
      verifiedChunks: chunks,
    );
  }

  Future<ContentDiscardReceipt> _discard(
    String method,
    Uint8List plan,
    CancellationToken cancellation,
  ) async {
    if (plan.isEmpty || plan.length > contentDiscardMaximumPlanBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final value = await _discardCall(method, {
      'plan': Uint8List.fromList(plan),
    }, cancellation);
    if (value is! Map ||
        value.length != 3 ||
        value['state'] is! String ||
        value['removedBytes'] is! int ||
        value['removedFiles'] is! int) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final state = ContentDiscardState.values
        .where((s) => s.name == value['state'])
        .firstOrNull;
    final bytes = value['removedBytes'] as int,
        files = value['removedFiles'] as int;
    if (state == null ||
        (method == 'discard' && state == ContentDiscardState.ready) ||
        bytes < 0 ||
        bytes > managedStageMaximumBytes ||
        files < 0 ||
        files > managedStageMaximumBytes ~/ managedStageChunkBytes + 1) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return ContentDiscardReceipt(
      state: state,
      removedBytes: bytes,
      removedFiles: files,
    );
  }

  @override
  Future<ContentDiscardReceipt> discard(
    Uint8List plan,
    CancellationToken cancellation,
  ) => _discard('discard', plan, cancellation);
  @override
  Future<ContentDiscardReceipt> inspectDiscard(
    Uint8List plan,
    CancellationToken cancellation,
  ) => _discard('inspectDiscard', plan, cancellation);
  @override
  Future<void> cancel() => _channel.invokeMethod<void>('cancel');
  @override
  Future<void> drain() {
    // Stage finalizers and lifecycle owners may wait on the same worker.
    // Share only settlement, never cancellation or the operation result.
    return _draining ??= _channel.invokeMethod<void>('drain').whenComplete(() {
      _draining = null;
    });
  }

  @override
  Future<void> close() {
    _closed = true;
    return _closing ??= _channel.invokeMethod<void>('close');
  }
}
