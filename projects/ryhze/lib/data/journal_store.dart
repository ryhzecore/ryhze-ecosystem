import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import '../core/app_failure.dart';
import 'store_schema.dart';
import 'transaction_store.dart';

/// A bounded append-only transaction journal for non-secret user intent.
/// A flushed newline terminates a commit; an interrupted trailing write is
/// backed up before truncation. Complete malformed records fail closed. The
/// checksum detects accidental damage, and is NOT an authenticity signature.
/// Native lock/flush/junction behavior still requires Windows acceptance tests.
class JournalStore implements TransactionStore {
  JournalStore._(
    this._file,
    this._handle,
    this._data,
    this._sequence,
    this._limit,
  );
  static const maxBytes = 16 * 1024 * 1024;
  static final Set<String> _openPaths = {};
  final int _limit;
  final File _file;
  final RandomAccessFile _handle;
  StoreData _data;
  int _sequence;
  Future<void> _pending = Future.value();
  bool _closed = false;
  bool _poisoned = false;
  bool _closing = false;
  @override
  bool get durable => true;

  static int checksum(String text) {
    var value = 0x811c9dc5;
    for (final byte in utf8.encode(text)) {
      value = ((value ^ byte) * 0x01000193) & 0xffffffff;
    }
    return value;
  }

  static Future<JournalStore> open(
    Directory directory, {
    int byteLimit = maxBytes,
  }) async {
    if (byteLimit < 1024 || byteLimit > maxBytes) {
      throw const AppFailure(FailureKind.invalidData);
    }
    // The composition root alone supplies this app-owned directory, never a
    // provider/catalog path. Reject existing link components before opening.
    var current = directory.absolute;
    while (true) {
      if (await FileSystemEntity.type(current.path, followLinks: false) ==
          FileSystemEntityType.link) {
        throw const AppFailure(FailureKind.denied);
      }
      if (current.parent.path == current.path) break;
      current = current.parent;
    }
    await directory.create(recursive: true);
    final root = await directory.resolveSymbolicLinks();
    final file = File('$root${Platform.pathSeparator}state.journal');
    if (await FileSystemEntity.type(file.path, followLinks: false) ==
            FileSystemEntityType.link ||
        !_openPaths.add(file.path)) {
      throw const AppFailure(FailureKind.conflict);
    }
    RandomAccessFile? handle;
    try {
      handle = await file.open(mode: FileMode.append);
      await handle.lock(FileLock.exclusive);
      final recovery = await _recoverCompaction(file, handle, byteLimit);
      final bytes = recovery.bytes;
      final lastNewline = bytes.lastIndexOf(10);
      final committed = bytes.sublist(0, lastNewline + 1);
      var data = <String, Object?>{
        'schema': currentStoreSchema,
        'accounts': <String, Object?>{},
        'preferences': <String, Object?>{},
      };
      var sequence = 0;
      for (final line in const LineSplitter().convert(utf8.decode(committed))) {
        final envelope = jsonDecode(line) as Map<String, dynamic>;
        final payload = envelope['payload'] as String;
        if (envelope['sequence'] != sequence + 1 ||
            envelope['checksum'] != checksum(payload)) {
          throw const AppFailure(FailureKind.recovery);
        }
        data = Map<String, Object?>.from(jsonDecode(payload) as Map);
        final schema = data['schema'];
        if (schema != 0 && schema != 1 && schema != currentStoreSchema) {
          throw const AppFailure(FailureKind.recovery);
        }
        if (data['accounts'] is! Map || data['preferences'] is! Map) {
          throw const AppFailure(FailureKind.recovery);
        }
        validateStoreSnapshot(data);
        sequence++;
      }
      if (lastNewline + 1 != bytes.length) {
        await _writeBackup(file, 'recovery', bytes);
        await handle.truncate(lastNewline + 1);
        await handle.setPosition(lastNewline + 1);
        await handle.flush();
      }
      final store = JournalStore._(file, handle, data, sequence, byteLimit);
      // v0/v1 remain readable and are backed up before adopting schema 2.
      // Explicit job intent identities require older binaries to refuse this store.
      if (data['schema'] != currentStoreSchema) {
        await _writeBackup(file, 'schema${data['schema']}', committed);
        await store.transact((draft) => draft['schema'] = currentStoreSchema);
      }
      // Keep the original temporary input until parsing, migration and all
      // required backups succeed. Its exclusive backup is already durable.
      final temporary = recovery.temporary;
      if (temporary != null) await temporary.delete();
      return store;
    } catch (_) {
      await handle?.close();
      _openPaths.remove(file.path);
      throw const AppFailure(FailureKind.recovery);
    }
  }

  @override
  StoreData read() => cloneData(_data);
  @override
  Future<void> transact(void Function(StoreData) update) {
    if (_closing) return Future.error(const AppFailure(FailureKind.storage));
    final operation = _pending.then((_) async {
      if (_closed || _poisoned) throw const AppFailure(FailureKind.storage);
      final draft = cloneData(_data);
      update(draft);
      validateStoreSnapshot(draft);
      if (draft['schema'] != currentStoreSchema) {
        throw const AppFailure(FailureKind.invalidData);
      }
      var bytes = _encode(draft, _sequence + 1);
      if (bytes.length > _limit ~/ 2) {
        throw const AppFailure(FailureKind.storage);
      }
      if (await _handle.length() + bytes.length > _limit) {
        await _compact();
        bytes = _encode(draft, _sequence + 1);
      }
      try {
        await _handle.writeFrom(bytes);
        await _handle.flush();
      } catch (_) {
        _poisoned = true;
        throw const AppFailure(FailureKind.storage);
      }
      _data = cloneData(draft);
      _sequence++;
    });
    _pending = operation.catchError((Object _) {});
    return operation;
  }

  static List<int> _encode(StoreData data, int sequence) {
    final payload = jsonEncode(data);
    return utf8.encode(
      '${jsonEncode({'sequence': sequence, 'checksum': checksum(payload), 'payload': payload})}\n',
    );
  }

  static Future<void> _rejectLink(File file) async {
    if (await FileSystemEntity.type(file.path, followLinks: false) ==
        FileSystemEntityType.link) {
      throw const AppFailure(FailureKind.denied);
    }
  }

  static Future<void> _writeBackup(
    File journal,
    String purpose,
    List<int> bytes,
  ) async {
    final backup = File(
      '${journal.path}.$purpose-${DateTime.now().microsecondsSinceEpoch}',
    );
    // Never reuse an existing backup or follow a pre-existing destination link.
    // A collision aborts recovery before the committed journal is changed.
    await _writeExclusive(backup, bytes);
  }

  static Future<void> _writeExclusive(File file, List<int> bytes) async {
    await file.create(exclusive: true);
    await _rejectLink(file);
    await file.writeAsBytes(bytes, flush: true);
  }

  static Future<Uint8List> _readRecoveryFile(File file, int limit) async {
    final handle = await file.open();
    try {
      return await _readLocked(handle, limit);
    } finally {
      await handle.close();
    }
  }

  // A stale checkpoint must not hide evidence that a newer writer owns the
  // main journal. Inspect each complete, checksum-valid record independently:
  // interrupted or malformed neighbors are not permission to downgrade it.
  static void _rejectNewerSchema(Uint8List bytes) {
    final committed = bytes.sublist(0, bytes.lastIndexOf(10) + 1);
    for (final line in const LineSplitter().convert(
      utf8.decode(committed, allowMalformed: true),
    )) {
      Object? schema;
      try {
        final envelope = jsonDecode(line);
        if (envelope is! Map) continue;
        final payload = envelope['payload'];
        if (payload is! String || envelope['checksum'] != checksum(payload)) {
          continue;
        }
        final snapshot = jsonDecode(payload);
        if (snapshot is! Map) continue;
        schema = snapshot['schema'];
      } on FormatException {
        continue;
      }
      if (schema is num && schema > currentStoreSchema) {
        throw const AppFailure(FailureKind.recovery);
      }
    }
  }

  // Windows locks belong to a handle: opening a second reader fails against
  // our own exclusive lock. On POSIX, closing that reader can release the
  // process's advisory lock. Keep all journal IO on the original handle.
  static Future<Uint8List> _readLocked(
    RandomAccessFile handle,
    int limit,
  ) async {
    final length = await handle.length();
    if (length > limit) throw const AppFailure(FailureKind.storage);
    final position = await handle.position();
    try {
      await handle.setPosition(0);
      final bytes = Uint8List(length);
      var offset = 0;
      while (offset < length) {
        final count = await handle.readInto(bytes, offset);
        if (count == 0) throw const AppFailure(FailureKind.recovery);
        offset += count;
      }
      return bytes;
    } finally {
      await handle.setPosition(position);
    }
  }

  static Future<({Uint8List bytes, File? temporary})> _recoverCompaction(
    File file,
    RandomAccessFile handle,
    int limit,
  ) async {
    final checkpoint = File('${file.path}.checkpoint');
    final temporary = File('${checkpoint.path}.tmp');
    await _rejectLink(checkpoint);
    await _rejectLink(temporary);
    final original = await _readLocked(handle, limit);
    _rejectNewerSchema(original);
    Uint8List? checkpointBytes;
    if (await checkpoint.exists()) {
      final bytes = await _readRecoveryFile(checkpoint, limit);
      if (bytes.isEmpty || bytes.length > limit || bytes.last != 10) {
        throw const AppFailure(FailureKind.recovery);
      }
      final lines = const LineSplitter().convert(utf8.decode(bytes));
      if (lines.length != 1) throw const AppFailure(FailureKind.recovery);
      final envelope = jsonDecode(lines.single) as Map;
      final payload = envelope['payload'] as String;
      if (envelope['sequence'] != 1 ||
          envelope['checksum'] != checksum(payload)) {
        throw const AppFailure(FailureKind.recovery);
      }
      validateStoreSnapshot(
        Map<String, Object?>.from(jsonDecode(payload) as Map),
      );
      checkpointBytes = bytes;
    }
    File? retainedTemporary;
    if (await temporary.exists()) {
      final bytes = await _readRecoveryFile(temporary, limit);
      // Reserve and flush the quarantine before restoring the main journal.
      // Rename can overwrite a colliding destination; an exclusive copy cannot.
      await _writeBackup(temporary, 'recovery', bytes);
      retainedTemporary = temporary;
    }
    if (checkpointBytes != null) {
      // A checkpoint is created before truncation and removed before any later
      // transaction. Preserve the interrupted main file before restoring it.
      await _writeBackup(file, 'recovery', original);
      await handle.truncate(0);
      await handle.setPosition(0);
      await handle.writeFrom(checkpointBytes);
      await handle.flush();
      await checkpoint.delete();
    }
    return (bytes: checkpointBytes ?? original, temporary: retainedTemporary);
  }

  Future<void> _compact() async {
    final checkpoint = File('${_file.path}.checkpoint');
    final temporary = File('${checkpoint.path}.tmp');
    try {
      await _rejectLink(checkpoint);
      await _rejectLink(temporary);
      if (await checkpoint.exists() || await temporary.exists()) {
        throw const AppFailure(FailureKind.recovery);
      }
      final snapshot = _encode(_data, 1);
      await _writeExclusive(temporary, snapshot);
      // Publishing by rename would replace an existing destination. Reserve a
      // new checkpoint and flush it before touching the main journal instead.
      // An interrupted copy leaves main intact and recovery fails closed on the
      // partial checkpoint; this is not an atomic publication guarantee.
      await _writeExclusive(checkpoint, snapshot);
      await temporary.delete();
      // Keep the original file handle and its exclusive lock across compaction.
      await _handle.truncate(0);
      await _handle.setPosition(0);
      await _handle.writeFrom(snapshot);
      await _handle.flush();
      await checkpoint.delete();
      _sequence = 1;
    } catch (_) {
      _poisoned = true;
      throw const AppFailure(FailureKind.recovery);
    }
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _closing = true;
    await _pending;
    await _handle.close();
    _closed = true;
    _openPaths.remove(_file.path);
  }
}
