import 'dart:async';
import 'dart:collection';

import 'package:flutter/foundation.dart';

import '../core/app_failure.dart';
import '../design/artwork_reference.dart';
import '../design/artwork_source.dart';
import 'public_artwork_cache.dart';
import 'public_artwork_decoder.dart';
import 'public_artwork_png.dart';
import 'public_artwork_transport.dart';

/// Anonymous presentation bytes only. Requests coalesce by exact identity;
/// each subscriber owns cancellation, and the last subscriber aborts the work.
class PublicArtworkRepository implements ArtworkSource, ArtworkMaintenance {
  PublicArtworkRepository({
    PublicArtworkTransport? transport,
    this.cache,
    DateTime Function()? clock,
    Future<void> Function(Uint8List)? validateDecoded,
  }) : transport = transport ?? PublicArtworkTransport(),
       clock = clock ?? DateTime.now,
       validateDecoded = validateDecoded ?? validateDecodedPublicArtwork;

  static const freshness = Duration(hours: 1);
  static const retention = Duration(days: 7);
  static const maxMemoryBytes = 16 * 1024 * 1024;
  static const maxMemoryEntries = 8;
  static const maxConcurrent = 2;
  static const maxQueued = 8;
  final PublicArtworkTransport transport;
  final PublicArtworkCache? cache;
  final DateTime Function() clock;
  final Future<void> Function(Uint8List) validateDecoded;
  final _changes = ValueNotifier(0);
  @override
  ValueListenable<int> get changes => _changes;
  final _memory = <String, PublicArtworkCached>{};
  final _jobs = <String, _ArtworkJob>{};
  final _queue = Queue<_ArtworkJob>();
  final _running = <Future<void>>{};
  int _memoryBytes = 0;
  bool _closed = false, _clearing = false, _invalidating = false;
  Future<void>? _closing;
  Future<PublicArtworkClearResult>? _clearFuture;
  AppFailure? _diskFailure;

  @override
  Future<ArtworkRead> read(
    ArtworkReference reference,
    CancellationToken token,
  ) async {
    token.check();
    if (_closed) throw const AppFailure(FailureKind.cancelled);
    if (_clearing || _invalidating) {
      throw const AppFailure(FailureKind.conflict);
    }
    var job = _jobs[reference.key];
    if (job != null && (job.token.isCancelled || job.subscribers >= 32)) {
      throw const AppFailure(FailureKind.conflict);
    }
    if (job == null) {
      if (_jobs.length >= maxConcurrent + maxQueued) {
        throw const AppFailure(FailureKind.conflict);
      }
      job = _ArtworkJob(reference);
      _jobs[reference.key] = job;
      _queue.add(job);
    }
    final subscribed = job;
    subscribed.subscribers++;
    final cancelled = Completer<ArtworkRead>();
    final remove = token.onCancel(() {
      if (!cancelled.isCompleted) {
        cancelled.completeError(const AppFailure(FailureKind.cancelled));
      }
    });
    // Attach before starting work so even a synchronously cancelled queued job
    // always has an error consumer.
    final result = Future.any([subscribed.result.future, cancelled.future]);
    _pump();
    try {
      final value = await result;
      token.check();
      return value;
    } finally {
      remove();
      if (--subscribed.subscribers == 0) {
        subscribed.token.cancel();
        _pump();
      }
    }
  }

  void _pump() {
    if (_invalidating) return;
    while (_running.length < maxConcurrent && _queue.isNotEmpty) {
      final job = _queue.removeFirst();
      late final Future<void> running;
      running = _run(job).whenComplete(() {
        _running.remove(running);
        _jobs.remove(job.reference.key);
        _pump();
      });
      _running.add(running);
    }
  }

  Future<void> _run(_ArtworkJob job) async {
    try {
      final result = await _load(job.reference, job.token);
      job.token.check();
      job.result.complete(result);
    } catch (error) {
      job.result.completeError(
        error is AppFailure ? error : const AppFailure(FailureKind.invalidData),
      );
    }
  }

  bool _usable(PublicArtworkCached value) {
    final age = clock().toUtc().difference(value.storedAt);
    return !age.isNegative && age <= retention;
  }

  bool _fresh(PublicArtworkCached value) =>
      _usable(value) && clock().toUtc().difference(value.storedAt) < freshness;

  void _remember(String key, PublicArtworkCached value) {
    final replaced = _memory.remove(key);
    _memoryBytes -= replaced?.bytes.length ?? 0;
    while (_memory.isNotEmpty &&
        (_memory.length >= maxMemoryEntries ||
            _memoryBytes + value.bytes.length > maxMemoryBytes)) {
      _memoryBytes -= _memory.remove(_memory.keys.first)!.bytes.length;
    }
    _memory[key] = value;
    _memoryBytes += value.bytes.length;
  }

  Future<void> _validate(Uint8List bytes, CancellationToken token) async {
    token.check();
    if (bytes.length > publicArtworkMaximumBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
    // CRC and chunk scanning can cover 8 MiB; keep that work off the UI isolate.
    await compute(
      validatePublicArtworkPng,
      bytes,
      debugLabel: 'ryhze-public-artwork-validation',
    );
    token.check();
    await validateDecoded(bytes);
    token.check();
  }

  Future<ArtworkRead> _load(
    ArtworkReference reference,
    CancellationToken token,
  ) async {
    token.check();
    var saved = _memory.remove(reference.key);
    var origin = ArtworkOrigin.memory;
    if (saved != null) {
      _memoryBytes -= saved.bytes.length;
      if (_usable(saved)) {
        _remember(reference.key, saved);
      } else {
        saved = null;
      }
    }
    if (saved == null && cache != null) {
      try {
        final disk = await cache!.get(reference.key);
        token.check();
        if (disk != null && _usable(disk)) {
          await _validate(disk.bytes, token);
          saved = disk;
          origin = ArtworkOrigin.disk;
          _remember(reference.key, disk);
        }
      } catch (_) {
        token.check();
        _diskFailure = const AppFailure(FailureKind.storage);
      }
    }
    if (saved != null && _fresh(saved)) {
      return ArtworkRead(
        bytes: saved.bytes,
        storedAt: saved.storedAt,
        origin: origin,
      );
    }
    try {
      // Take ownership of the transport's mutable buffer before validation.
      final fetched = await transport.fetch(reference, token);
      token.check();
      if (fetched.length > publicArtworkMaximumBytes) {
        throw const AppFailure(FailureKind.invalidData);
      }
      final bytes = Uint8List.fromList(fetched).asUnmodifiableView();
      await _validate(bytes, token);
      final now = clock().toUtc();
      final value = PublicArtworkCached(bytes, now);
      if (cache != null) {
        try {
          await cache!.put(reference.key, value.bytes, now);
        } catch (_) {
          _diskFailure = const AppFailure(FailureKind.storage);
        }
        token.check();
      }
      _remember(reference.key, value);
      return ArtworkRead(
        bytes: value.bytes,
        storedAt: now,
        origin: ArtworkOrigin.network,
      );
    } catch (error) {
      token.check();
      final failure = error is AppFailure
          ? error
          : const AppFailure(FailureKind.invalidData);
      // Only a transient connection failure permits stale display. Revoked,
      // malformed or denied content must not silently resurrect an old image.
      if (failure.kind == FailureKind.network &&
          saved != null &&
          _usable(saved)) {
        return ArtworkRead(
          bytes: saved.bytes,
          storedAt: saved.storedAt,
          origin: origin,
          stale: true,
        );
      }
      throw failure;
    }
  }

  @override
  Future<PublicArtworkHealth> health() async {
    PublicArtworkCacheHealth? disk;
    if (!_closed && cache != null) {
      try {
        disk = await cache!.health();
        _diskFailure = null;
      } catch (_) {
        _diskFailure = const AppFailure(FailureKind.storage);
      }
    }
    return PublicArtworkHealth(
      memoryEntries: _memory.length,
      memoryBytes: _memoryBytes,
      active: _running.length,
      queued: _queue.length,
      disk: disk,
      diskFailure: _diskFailure,
    );
  }

  void interrupt() {
    if (_closed) return;
    _invalidate();
  }

  void _invalidate() {
    if (_invalidating) return;
    _invalidating = true;
    (Object, StackTrace)? failure;
    try {
      // Cancellation can settle a transport synchronously and remove its job.
      // Cancel the whole snapshot before any queued request can be started.
      for (final job in List.of(_jobs.values)) {
        try {
          job.token.cancel();
        } catch (error, stack) {
          failure ??= (error, stack);
        }
      }
      _memory.clear();
      _memoryBytes = 0;
      _changes.value++;
    } finally {
      _invalidating = false;
      _pump();
    }
    if (failure != null) Error.throwWithStackTrace(failure.$1, failure.$2);
  }

  Future<void> _settle() async {
    while (_invalidating || _running.isNotEmpty || _queue.isNotEmpty) {
      _pump();
      await Future.wait(List.of(_running));
    }
    await cache?.drain();
  }

  @override
  Future<PublicArtworkClearResult> clear() {
    if (_closed) return Future.error(const AppFailure(FailureKind.cancelled));
    if (_clearing) return Future.error(const AppFailure(FailureKind.conflict));
    _clearing = true;
    // Publish ownership before cancellation/notification callbacks can close us.
    final completion = Completer<PublicArtworkClearResult>();
    _clearFuture = completion.future;
    unawaited(
      _clear().then(completion.complete, onError: completion.completeError),
    );
    return completion.future;
  }

  Future<PublicArtworkClearResult> _clear() async {
    try {
      (Object, StackTrace)? cancellationFailure;
      try {
        _invalidate();
      } catch (error, stack) {
        cancellationFailure = (error, stack);
      }
      // Join every previous put before deleting, including timed-out calls.
      await _settle();
      if (cancellationFailure != null) {
        Error.throwWithStackTrace(
          cancellationFailure.$1,
          cancellationFailure.$2,
        );
      }
      final result =
          await cache?.clear() ??
          const PublicArtworkClearResult(removed: 0, preserved: 0);
      _diskFailure = null;
      return result;
    } catch (_) {
      _diskFailure = const AppFailure(FailureKind.storage);
      throw _diskFailure!;
    } finally {
      _clearing = false;
    }
  }

  Future<void> close() {
    if (_closing != null) return _closing!;
    _closed = true;
    final completion = Completer<void>();
    _closing = completion.future;
    unawaited(
      _close().then(completion.complete, onError: completion.completeError),
    );
    return completion.future;
  }

  Future<void> _close() async {
    try {
      _invalidate();
      await _clearFuture;
    } finally {
      try {
        await _settle();
      } finally {
        _changes.dispose();
      }
    }
  }
}

class _ArtworkJob {
  _ArtworkJob(this.reference);
  final ArtworkReference reference;
  final token = CancellationToken();
  final result = Completer<ArtworkRead>();
  int subscribers = 0;
}
