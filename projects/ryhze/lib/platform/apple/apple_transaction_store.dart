import '../../core/app_failure.dart';
import '../../data/store_schema.dart';
import '../../data/transaction_store.dart';
import 'apple_platform_client.dart';

/// The Apple persistence adapter for the existing shared application snapshot.
/// Account isolation, preferences, and jobs remain owned by shared repositories.
final class AppleTransactionStore implements TransactionStore {
  AppleTransactionStore._(this._client, AppleStoreSnapshot snapshot)
    : _data = cloneData(snapshot.data),
      _revision = snapshot.revision;

  final ApplePlatformClient _client;
  StoreData _data;
  int _revision;
  bool _closing = false, _closed = false, _poisoned = false;
  Future<void> _pending = Future.value();
  Future<void>? _closeFuture;

  static Future<AppleTransactionStore> open({
    ApplePlatformClient? client,
  }) async {
    final native = client ?? const ApplePlatformClient();
    try {
      await native.bootstrap();
      var snapshot = await native.readState();
      if (snapshot.data['schema'] != currentStoreSchema) {
        snapshot = await native.migrateState(snapshot);
      }
      // Validate injectable clients as well as the real channel response.
      ApplePlatformClient.encodeSnapshot(snapshot.data);
      return AppleTransactionStore._(native, snapshot);
    } on ApplePlatformException catch (error) {
      throw error.failure;
    } on AppFailure {
      rethrow;
    } catch (_) {
      throw const AppFailure(FailureKind.recovery);
    }
  }

  @override
  bool get durable => true;

  @override
  StoreData read() {
    if (_closed) throw const AppFailure(FailureKind.storage);
    return cloneData(_data);
  }

  @override
  Future<void> transact(void Function(StoreData) update) {
    if (_closing) return Future.error(const AppFailure(FailureKind.storage));
    final operation = _pending.then((_) async {
      if (_closed || _poisoned) throw const AppFailure(FailureKind.storage);
      final StoreData draft;
      try {
        draft = cloneData(_data);
        update(draft);
        ApplePlatformClient.encodeSnapshot(draft);
      } on AppFailure {
        rethrow;
      } catch (_) {
        throw const AppFailure(FailureKind.invalidData);
      }

      try {
        final committed = await _client.compareAndSetState(_revision, draft);
        if (committed.revision != _revision + 1) {
          throw const ApplePlatformException('corruptState');
        }
        ApplePlatformClient.encodeSnapshot(committed.data);
        _data = cloneData(committed.data);
        _revision = committed.revision;
      } catch (error) {
        // A lost reply can mean a committed write. Stop further writes until
        // reopening rather than replaying an arbitrary transaction callback.
        _poisoned = true;
        if (error is ApplePlatformException) throw error.failure;
        if (error is AppFailure) rethrow;
        throw const AppFailure(FailureKind.storage);
      }
    });
    _pending = operation.catchError((Object _) {});
    return operation;
  }

  @override
  Future<void> close() {
    _closing = true;
    return _closeFuture ??= _pending.then((_) {
      _closed = true;
    });
  }
}
