import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// Native power events are distinct from window visibility/focus. They carry no
/// account, file, command or credential authority.
enum WindowsLifecycleState { active, suspended, resumed, ending }

class WindowsLifecycleEvent {
  const WindowsLifecycleEvent(this.sequence, this.state);
  final int sequence;
  final WindowsLifecycleState state;
}

/// Optional, versioned Windows bridge. Connects only when the native host proves
/// this exact capability; an OS label alone never enables an operation.
class WindowsLifecycleClient extends ValueNotifier<WindowsLifecycleEvent> {
  WindowsLifecycleClient._(this._channel)
    : super(const WindowsLifecycleEvent(0, WindowsLifecycleState.active));

  final MethodChannel _channel;
  bool _closed = false;

  static Future<WindowsLifecycleClient?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.lifecycle.v1',
    ),
  }) async {
    final client = WindowsLifecycleClient._(channel);
    channel.setMethodCallHandler(client._receive);
    try {
      final snapshot = await channel
          .invokeMethod<Object?>('connect')
          .timeout(const Duration(seconds: 2));
      if (snapshot is! Map ||
          snapshot.length != 4 ||
          snapshot['schemaVersion'] is! int ||
          snapshot['schemaVersion'] != 1 ||
          snapshot['powerEvents'] != true ||
          !snapshot.keys.every(
            const {
              'schemaVersion',
              'powerEvents',
              'sequence',
              'state',
            }.contains,
          )) {
        throw const FormatException();
      }
      client._apply(
        _parse({'sequence': snapshot['sequence'], 'state': snapshot['state']}),
      );
      return client;
    } catch (_) {
      // An optional host integration never replaces durable storage with a
      // memory fallback or exposes native exception details to the interface.
      client.dispose();
      return null;
    }
  }

  static WindowsLifecycleEvent _parse(Object? value) {
    if (value is! Map ||
        value.length != 2 ||
        !value.keys.every(const {'sequence', 'state'}.contains) ||
        value['sequence'] is! int ||
        (value['sequence'] as int) < 0) {
      throw const FormatException();
    }
    final state = switch (value['state']) {
      'active' => WindowsLifecycleState.active,
      'suspended' => WindowsLifecycleState.suspended,
      'resumed' => WindowsLifecycleState.resumed,
      'ending' => WindowsLifecycleState.ending,
      _ => throw const FormatException(),
    };
    if (value['sequence'] == 0 && state != WindowsLifecycleState.active) {
      throw const FormatException();
    }
    return WindowsLifecycleEvent(value['sequence'] as int, state);
  }

  void _apply(WindowsLifecycleEvent event) {
    if (_closed || event.sequence <= value.sequence) return;
    value = event;
  }

  Future<void> _receive(MethodCall call) async {
    if (_closed) return;
    if (call.method != 'lifecycle') throw MissingPluginException();
    try {
      _apply(_parse(call.arguments));
    } on FormatException {
      throw PlatformException(code: 'invalidArguments');
    }
  }

  @override
  void dispose() {
    if (_closed) return;
    _closed = true;
    _channel.setMethodCallHandler(null);
    super.dispose();
  }
}
