import '../../core/app_failure.dart';
import '../../data/transaction_store.dart';
import '../session/session_controller.dart';

class SavedTitle {
  const SavedTitle(this.id, this.title);
  final String id;
  final String title;
}

/// Local user intent only. Never read by an entitlement projection. Exact keys
/// include account authority/reference. No remote writes/outbox is implied.
class CollectionRepository {
  CollectionRepository(this.store);
  final TransactionStore store;
  List<SavedTitle> read(AccountScope scope) {
    final accounts = store.read()['accounts'] as Map;
    final account = accounts[scope.key] as Map?;
    final saved = account?['saved'] as Map? ?? {};
    return List.unmodifiable(
      saved.entries.map((e) => SavedTitle(e.key as String, e.value as String)),
    );
  }

  Future<void> setSaved(AccountScope scope, SavedTitle title, bool saved) {
    if (title.id.isEmpty ||
        title.id.length > 256 ||
        title.title.trim().isEmpty ||
        title.title.length > 512 ||
        RegExp(r'[\x00-\x1f]').hasMatch(title.id + title.title)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    return store.transact((draft) {
      final accounts = draft['accounts'] as Map;
      final account =
          accounts.putIfAbsent(scope.key, () => <String, Object?>{}) as Map;
      final titles =
          account.putIfAbsent('saved', () => <String, Object?>{}) as Map;
      if (saved) {
        if (titles.length >= 5000 && !titles.containsKey(title.id)) {
          throw const AppFailure(FailureKind.storage);
        }
        titles[title.id] = title.title;
      } else {
        titles.remove(title.id);
      }
    });
  }

  Future<void> clear(AccountScope scope) => store.transact((draft) {
    final account = (draft['accounts'] as Map)[scope.key] as Map?;
    account?.remove('saved');
  });
}

class Preferences {
  const Preferences({
    this.reduceMotion = false,
    this.privatePreviews = true,
    this.notifications = false,
  });
  final bool reduceMotion;
  final bool privatePreviews;
  final bool notifications;
}

class PreferenceRepository {
  PreferenceRepository(this.store);
  final TransactionStore store;
  Preferences read() {
    final values = store.read()['preferences'] as Map;
    return Preferences(
      reduceMotion: values['reduceMotion'] == true,
      privatePreviews: values['privatePreviews'] != false,
      notifications: values['notifications'] == true,
    );
  }

  Future<void> save(Preferences value) => store.transact((draft) {
    draft['preferences'] = {
      'reduceMotion': value.reduceMotion,
      'privatePreviews': value.privatePreviews,
      'notifications': value.notifications,
    };
  });
}
