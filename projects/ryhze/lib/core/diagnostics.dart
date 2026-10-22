import 'dart:convert';

enum DiagnosticEvent {
  startup,
  storageUnavailable,
  scopeReplaced,
  preferenceSaved,
  operationFailed,
}

/// No arbitrary strings or exception payloads are accepted into diagnostic events.
class Diagnostics {
  final List<Map<String, Object>> _events = [];
  int _sequence = 0;
  void record(DiagnosticEvent event) {
    _events.add({
      'event': event.name,
      'correlation': ++_sequence,
      'time': DateTime.now().toUtc().toIso8601String(),
    });
    if (_events.length > 100) _events.removeAt(0);
  }

  String export() =>
      const JsonEncoder.withIndent('  ')
          .convert({'format': 1, 'events': _events});
}
