import 'dart:io';

/// Resolve only the OS-provided temporary base before creating app-owned data.
/// macOS can expose that base through system aliases such as /var. JournalStore
/// still rejects links in every component of the new fixture and its files.
Future<Directory> createJournalFixture(String prefix) async {
  final base = Directory(await Directory.systemTemp.resolveSymbolicLinks());
  return base.createTemp(prefix);
}
