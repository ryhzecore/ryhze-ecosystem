import 'dart:io';

import '../core/app_failure.dart';
import '../data/journal_store.dart';
import '../data/transaction_store.dart';
import '../platform/apple/apple_transaction_store.dart';

Future<TransactionStore> openApplicationStore() async {
  if (Platform.isIOS || Platform.isMacOS) {
    return AppleTransactionStore.open();
  }
  final String? base;
  if (Platform.isWindows) {
    base = Platform.environment['LOCALAPPDATA'];
  } else if (Platform.isLinux) {
    base =
        Platform.environment['XDG_DATA_HOME'] ??
        (Platform.environment['HOME'] == null
            ? null
            : '${Platform.environment['HOME']}/.local/share');
  } else {
    return MemoryTransactionStore();
  }
  if (base == null || base.isEmpty || !Directory(base).isAbsolute) {
    throw const AppFailure(FailureKind.storage);
  }
  return JournalStore.open(
    Directory(
      '$base${Platform.pathSeparator}Ryhze-Entertainment${Platform.pathSeparator}user-data',
    ),
  );
}
