import 'dart:io';

import 'windows_lifecycle.dart';

Future<WindowsLifecycleClient?> connectWindowsLifecycle() async =>
    Platform.isWindows ? WindowsLifecycleClient.connect() : null;
