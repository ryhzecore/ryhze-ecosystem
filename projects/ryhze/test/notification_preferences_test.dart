import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/data/transaction_store.dart';
import 'package:ryhze/features/collections/collection_repository.dart';

void main() {
  test('notification opt-in remains off for existing preferences and persists independently', () async {
    final store = MemoryTransactionStore();
    final preferences = PreferenceRepository(store);
    expect(preferences.read().notifications, isFalse);
    expect(preferences.read().privatePreviews, isTrue);
    await preferences.save(
      const Preferences(notifications: true, reduceMotion: true),
    );
    final reread = PreferenceRepository(store).read();
    expect(reread.notifications, isTrue);
    expect(reread.reduceMotion, isTrue);
    expect(reread.privatePreviews, isTrue);
    await store.close();
  });
}
