import 'dart:convert';

import '../core/app_failure.dart';
import '../domain/activity.dart';
import '../domain/app_route.dart';
import '../features/installations/managed_installation.dart';
import '../features/installations/managed_transfer_record.dart';

const currentStoreSchema = 2;
final _controlCharacters = RegExp(r'[\x00-\x1f]');

/// Validate non-secret persisted state before exposing repositories to widgets.
/// Unknown top-level fields survive migration, but known fields fail closed.
void validateStoreSnapshot(Map<String, Object?> data) {
  try {
    validateManagedInstallations(data['managedInstallations']);
    validateManagedTransfers(data['managedTransfers']);
    validateManagedContentReservations(data);
    if (data['schema'] != 0 &&
        data['schema'] != 1 &&
        data['schema'] != currentStoreSchema) {
      throw const FormatException();
    }
    final preferences = data['preferences'] as Map;
    for (final key in ['reduceMotion', 'privatePreviews', 'notifications']) {
      if (preferences.containsKey(key) && preferences[key] is! bool) {
        throw const FormatException();
      }
    }
    final accounts = data['accounts'] as Map;
    if (accounts.length > 100) throw const FormatException();
    for (final entry in accounts.entries) {
      final key = jsonDecode(entry.key as String) as List;
      if (key.length != 2 ||
          key.any(
            (part) =>
                part is! String ||
                part.isEmpty ||
                part.length > 256 ||
                _controlCharacters.hasMatch(part),
          )) {
        throw const FormatException();
      }
      final account = entry.value as Map;
      final saved = account['saved'] as Map? ?? {};
      if (saved.length > 5000) throw const FormatException();
      for (final title in saved.entries) {
        if (title.key is! String ||
            title.value is! String ||
            (title.key as String).isEmpty ||
            (title.key as String).length > 256 ||
            (title.value as String).trim().isEmpty ||
            (title.value as String).length > 512 ||
            RegExp(r'[\x00-\x1f]').hasMatch('${title.key}${title.value}')) {
          throw const FormatException();
        }
      }
      final favourites = account['libraryFavourites'] as List? ?? [];
      if (favourites.length > 5000 ||
          favourites.toSet().length != favourites.length) {
        throw const FormatException();
      }
      for (final encoded in favourites) {
        final identity = jsonDecode(encoded as String) as List;
        if (identity.length != 2 ||
            identity.any(
              (part) =>
                  part is! String ||
                  part.trim().isEmpty ||
                  part.length > 256 ||
                  _controlCharacters.hasMatch(part),
            )) {
          throw const FormatException();
        }
      }
      final activity = account['activity'] as Map? ?? {};
      if (activity.length > 200) throw const FormatException();
      for (final entry in activity.entries) {
        final key = jsonDecode(entry.key as String) as List;
        final record = entry.value as Map;
        if (key.length != 2 ||
            key[0] is! String ||
            (key[0] as String).isEmpty ||
            (key[0] as String).length > 1024 ||
            _controlCharacters.hasMatch(key[0] as String) ||
            key[1] != record['kind'] ||
            !ActivityKind.values.any((kind) => kind.name == record['kind']) ||
            record['count'] is! int ||
            (record['count'] as int) < 1 ||
            (record['count'] as int) > 1000000 ||
            record['acknowledged'] is! bool ||
            DateTime.tryParse(record['occurredAt'] as String) == null ||
            (record['route'] != null &&
                AppRoute.parse(Uri.parse(record['route'] as String)) == null)) {
          throw const FormatException();
        }
      }
      final jobs = account['jobs'] as Map? ?? {};
      if (jobs.length > 1000) throw const FormatException();
      for (final entry in jobs.entries) {
        final job = entry.value as Map;
        if (job['kind'] is! String ||
            job['item'] is! String ||
            (job['kind'] as String).isEmpty ||
            (job['kind'] as String).length > 80 ||
            (job['item'] as String).isEmpty ||
            (job['item'] as String).length > 256 ||
            _controlCharacters.hasMatch('${job['kind']}${job['item']}') ||
            entry.key !=
                jsonEncode([
                  job['kind'],
                  job['item'],
                  if (job['intent'] != null) job['intent'],
                ]) ||
            (job['intent'] != null &&
                (job['intent'] is! String ||
                    !RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')
                        .hasMatch(job['intent'] as String))) ||
            (job['failure'] != null &&
                !FailureKind.values.any(
                  (kind) => kind.name == job['failure'],
                )) ||
            !const {
              'queued',
              'running',
              'handoffAccepted',
              'completed',
              'cancelled',
              'failed',
              'recoveryRequired',
              'blocked',
            }.contains(job['state']) ||
            job['checkpoint'] is! int ||
            (job['checkpoint'] as int) < 0 ||
            DateTime.tryParse(job['updatedAt'] as String) == null) {
          throw const FormatException();
        }
      }
    }
  } catch (_) {
    throw const AppFailure(FailureKind.invalidData);
  }
}
