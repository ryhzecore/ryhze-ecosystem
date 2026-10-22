import 'dart:convert';

import '../../../data/transaction_store.dart';
import '../../session/session_controller.dart';
import '../model/library_entry.dart';

class LibraryPreferences {
  LibraryPreferences(this.storage, this.scope);
  final TransactionStore storage;
  final AccountScope scope;
  bool get durable => storage.durable;
  Set<LibraryIdentity> read() {
    final account = (storage.read()['accounts'] as Map)[scope.key] as Map?;
    final keys = account?['libraryFavourites'] as List? ?? [];
    return Set.unmodifiable(
      keys.map((encoded) {
        final key = jsonDecode(encoded as String) as List;
        return LibraryIdentity(
          providerId: key[0] as String,
          titleId: key[1] as String,
        );
      }),
    );
  }

  Future<void> save(Set<LibraryIdentity> values) => storage.transact((draft) {
    final account = (draft['accounts'] as Map).putIfAbsent(
      scope.key,
      () => <String, Object?>{},
    ) as Map;
    account['libraryFavourites'] =
        values.map((id) => jsonEncode([id.providerId, id.titleId])).toList()
          ..sort();
  });
}
