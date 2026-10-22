import 'dart:convert';

import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../../data/transaction_store.dart';
import '../../domain/activity.dart';
import '../../domain/app_route.dart';
import '../session/session_controller.dart';

/// Local, account-scoped activity. Closed event summaries cannot contain provider
/// payloads. Jobs call recordIn inside their transaction so outcome and event agree.
class ActivityRepository extends ChangeNotifier {
  ActivityRepository(this.store);
  final TransactionStore store;
  static const limit = 200;
  List<ActivityRecord> read(AccountScope scope) {
    final account = (store.read()['accounts'] as Map)[scope.key] as Map?;
    final records = account?['activity'] as Map? ?? {};
    final result =
        records.entries
            .map((e) => ActivityRecord.decode(e.key as String, e.value as Map))
            .toList()
          ..sort((a, b) => b.occurredAt.compareTo(a.occurredAt));
    return List.unmodifiable(result);
  }

  bool _disposed = false;
  void changed() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }

  static void recordIn(
    StoreData draft,
    AccountScope scope,
    String reference,
    ActivityKind kind,
    DateTime now, {
    AppRoute? route,
  }) {
    if (reference.isEmpty ||
        reference.length > 1024 ||
        RegExp(r'[\x00-\x1f]').hasMatch(reference)) {
      throw const AppFailure(FailureKind.invalidData);
    }
    final account = (draft['accounts'] as Map).putIfAbsent(
      scope.key,
      () => <String, Object?>{},
    ) as Map;
    final records =
        account.putIfAbsent('activity', () => <String, Object?>{}) as Map;
    final key = jsonEncode([reference, kind.name]);
    final old = records[key] as Map?;
    final oldTime = old == null
        ? null
        : DateTime.parse(old['occurredAt'] as String);
    final time = oldTime != null && oldTime.isAfter(now) ? oldTime : now;
    records[key] = {
      'kind': kind.name,
      'occurredAt': time.toUtc().toIso8601String(),
      'count': ((old?['count'] as int? ?? 0) + 1).clamp(1, 1000000),
      'acknowledged': false,
      if (route != null) 'route': route.toUri().toString(),
    };
    if (records.length > limit) {
      final oldest = records.keys.where((id) => id != key).toList()
        ..sort(
          (a, b) => (records[a]['occurredAt'] as String).compareTo(
            records[b]['occurredAt'] as String,
          ),
        );
      for (final id in oldest.take(records.length - limit)) {
        records.remove(id);
      }
    }
  }

  Future<void> acknowledge(AccountScope scope, String id) async {
    await store.transact((draft) {
      final account = (draft['accounts'] as Map)[scope.key] as Map?;
      final record = (account?['activity'] as Map?)?[id] as Map?;
      if (record != null) record['acknowledged'] = true;
    });
    changed();
  }

  Future<void> clearAcknowledged(AccountScope scope) async {
    await store.transact((draft) {
      final account = (draft['accounts'] as Map)[scope.key] as Map?;
      (account?['activity'] as Map?)?.removeWhere(
        (_, value) => (value as Map)['acknowledged'] == true,
      );
    });
    changed();
  }
}
