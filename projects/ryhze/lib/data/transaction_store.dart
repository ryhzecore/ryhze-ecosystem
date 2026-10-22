import 'dart:convert';

import '../core/app_failure.dart';
import 'store_schema.dart';

typedef StoreData = Map<String, Object?>;
StoreData cloneData(StoreData data) =>
    Map<String, Object?>.from(jsonDecode(jsonEncode(data)) as Map);

/// Single write owner. A transaction commits one complete snapshot. Repositories
/// receive copies, so a caller cannot mutate committed state outside a write.
abstract interface class TransactionStore {
  StoreData read();
  Future<void> transact(void Function(StoreData) update);
  Future<void> close();
  bool get durable;
}

class MemoryTransactionStore implements TransactionStore {
  StoreData _data = {
    'schema': currentStoreSchema,
    'accounts': <String, Object?>{},
    'preferences': <String, Object?>{},
  };
  Future<void> _pending = Future.value();
  bool _closed = false;
  @override
  bool get durable => false;
  @override
  StoreData read() => cloneData(_data);
  @override
  Future<void> transact(void Function(StoreData) update) {
    final next = _pending.then((_) {
      if (_closed) throw const AppFailure(FailureKind.storage);
      final draft = cloneData(_data);
      update(draft);
      validateStoreSnapshot(draft);
      _data = cloneData(draft);
    });
    _pending = next.catchError((Object _) {});
    return next;
  }

  @override
  Future<void> close() async {
    await _pending;
    _closed = true;
  }
}
