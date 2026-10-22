import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import '../../domain/app_route.dart';

class WindowsActivation {
  const WindowsActivation(this.sequence, this.route);
  final int sequence;
  final AppRoute? route;
}

/// The native host authenticates its IPC peers. Dart still validates every route;
/// messages can navigate or focus only, never install, purchase or launch.
class WindowsActivationClient extends ChangeNotifier {
  WindowsActivationClient._(this._channel);
  final MethodChannel _channel;
  static final _owners = Expando<Map<String, WindowsActivationClient>>();
  final Map<int, WindowsActivation> _pending = {};
  int _acknowledged = 0;
  bool _closed = false;
  List<WindowsActivation> get pending =>
      _pending.values.toList()
        ..sort((a, b) => a.sequence.compareTo(b.sequence));

  static Future<WindowsActivationClient?> connect({
    MethodChannel channel = const MethodChannel(
      'com.ryhze.ryhze/windows.activation.v1',
    ),
  }) async {
    final client = WindowsActivationClient._(channel);
    (_owners[channel.binaryMessenger] ??= {})[channel.name] = client;
    channel.setMethodCallHandler(client._receive);
    try {
      final value = await channel
          .invokeMethod<Object?>('connect')
          .timeout(const Duration(seconds: 2));
      if (value is! Map ||
          value.length != 3 ||
          value['schemaVersion'] is! int ||
          value['schemaVersion'] != 1 ||
          value['singleInstance'] != true ||
          value['activations'] is! List ||
          (value['activations'] as List).length > 16) {
        throw const FormatException();
      }
      for (final item in value['activations'] as List) {
        client._accept(item);
      }
      return client;
    } catch (_) {
      client.dispose();
      return null;
    }
  }

  void _accept(Object? value) {
    if (value is! Map ||
        value.length != 2 ||
        value['sequence'] is! int ||
        (value['sequence'] as int) < 1 ||
        !value.containsKey('uri')) {
      throw const FormatException();
    }
    final uri = value['uri'];
    AppRoute? route;
    if (uri != null) {
      if (uri is! String || uri.length > 4096) throw const FormatException();
      route = AppRoute.parse(Uri.parse(uri));
      if (route == null) throw const FormatException();
    }
    final sequence = value['sequence'] as int;
    if (sequence <= _acknowledged) return;
    final existing = _pending[sequence];
    if (existing != null) {
      if (existing.route?.toUri().toString() != route?.toUri().toString()) {
        throw const FormatException();
      }
      return;
    }
    if (_pending.length >= 16) throw const FormatException();
    _pending[sequence] = WindowsActivation(sequence, route);
  }

  Future<void> _receive(MethodCall call) async {
    if (_closed) return;
    if (call.method != 'activate') throw MissingPluginException();
    try {
      _accept(call.arguments);
    } catch (_) {
      throw PlatformException(code: 'invalidArguments');
    }
    notifyListeners();
  }

  Future<void> acknowledge(WindowsActivation event) async {
    if (_closed ||
        _pending[event.sequence] != event ||
        pending.first != event) {
      return;
    }
    await _channel.invokeMethod<void>('ack', {'sequence': event.sequence});
    if (_closed) return;
    _acknowledged = event.sequence;
    _pending.removeWhere((sequence, _) => sequence <= _acknowledged);
  }

  @override
  void dispose() {
    if (_closed) return;
    _closed = true;
    final owners = _owners[_channel.binaryMessenger];
    if (identical(owners?[_channel.name], this)) {
      owners!.remove(_channel.name);
      _channel.setMethodCallHandler(null);
    }
    _pending.clear();
    super.dispose();
  }
}
