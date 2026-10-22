import 'dart:convert';
import 'dart:io';

import 'package:ryhze/data/journal_store.dart';

// Run with the pinned Dart SDK. A separate process must observe the OS lock,
// independently of JournalStore's same-isolate ownership guard.
Future<void> main(List<String> args) async {
  if (args.length == 2 && args.first == '--contend') {
    final handle = await File(args.last).open(mode: FileMode.append);
    try {
      try {
        await handle.lock(FileLock.exclusive);
      } on FileSystemException {
        stdout.writeln('lock-denied');
        exitCode = 17;
        return;
      }
      stdout.writeln('lock-acquired');
    } finally {
      await handle.close();
    }
    return;
  }
  if (args.isNotEmpty) throw ArgumentError('No arguments expected.');
  final temp = Directory(await Directory.systemTemp.resolveSymbolicLinks());
  for (final scenario in ['reopen', 'trailing', 'migration', 'compaction']) {
    final root = await temp.createTemp('ryhze-lock-$scenario-');
    JournalStore? store;
    try {
      final file = File('${root.path}/state.journal');
      final payload = jsonEncode({
        'schema': scenario == 'migration' ? 1 : 2,
        'accounts': {},
        'preferences': {'reduceMotion': true},
      });
      final record =
          '${jsonEncode({'sequence': 1, 'checksum': JournalStore.checksum(payload), 'payload': payload})}\n';
      await file.writeAsString(
        scenario == 'compaction'
            ? '{interrupted'
            : '$record${scenario == 'trailing' ? '{partial' : ''}',
      );
      if (scenario == 'compaction') {
        await File('${file.path}.checkpoint').writeAsString(record);
      }
      store = await JournalStore.open(root, byteLimit: 2048);
      await _checkContender(file, locked: true);
      // Force live compaction while the original handle remains open.
      for (var i = 0; i < 40; i++) {
        await store.transact((draft) {
          draft['preferences'] = {'reduceMotion': i.isOdd};
        });
      }
      await _checkContender(file, locked: true);
      await store.close();
      store = null;
      await _checkContender(file, locked: false);
      stdout.writeln(
        '$scenario: lock held after open/recovery and compaction; '
        'released after close',
      );
    } finally {
      await store?.close();
      await root.delete(recursive: true);
    }
  }
}

Future<void> _checkContender(File file, {required bool locked}) async {
  final process = await Process.start(Platform.resolvedExecutable, [
    Platform.script.toFilePath(),
    '--contend',
    file.path,
  ]);
  final output = process.stdout.transform(utf8.decoder).join();
  final errors = process.stderr.transform(utf8.decoder).join();
  int code;
  try {
    code = await process.exitCode.timeout(const Duration(seconds: 30));
  } catch (_) {
    process.kill();
    rethrow;
  }
  final text = (await output).trim();
  final stderr = await errors;
  if (code != (locked ? 17 : 0) ||
      text != (locked ? 'lock-denied' : 'lock-acquired') ||
      stderr.isNotEmpty) {
    throw StateError(
      'Child lock assertion failed: code=$code, '
      'output=$text, stderr=$stderr',
    );
  }
}
