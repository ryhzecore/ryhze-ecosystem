import 'dart:ffi';

final class _IoCounters extends Struct {
  @Uint64()
  external int readOperations;
  @Uint64()
  external int writeOperations;
  @Uint64()
  external int otherOperations;
  @Uint64()
  external int readBytes;
  @Uint64()
  external int writeBytes;
  @Uint64()
  external int otherBytes;
}

/// Test-only counters for this process. No machine state or settings change.
Map<String, int> processCounters() {
  final kernel = DynamicLibrary.open('kernel32.dll');
  final runtime = DynamicLibrary.open('ucrtbase.dll');
  final allocate = runtime
      .lookupFunction<
        Pointer<Void> Function(IntPtr),
        Pointer<Void> Function(int)
      >('malloc');
  final release = runtime
      .lookupFunction<
        Void Function(Pointer<Void>),
        void Function(Pointer<Void>)
      >('free');
  final current = kernel
      .lookupFunction<Pointer<Void> Function(), Pointer<Void> Function()>(
        'GetCurrentProcess',
      )();
  final readIo = kernel
      .lookupFunction<
        Int32 Function(Pointer<Void>, Pointer<_IoCounters>),
        int Function(Pointer<Void>, Pointer<_IoCounters>)
      >('GetProcessIoCounters');
  final readTime = kernel
      .lookupFunction<
        Int32 Function(
          Pointer<Void>,
          Pointer<Uint64>,
          Pointer<Uint64>,
          Pointer<Uint64>,
          Pointer<Uint64>,
        ),
        int Function(
          Pointer<Void>,
          Pointer<Uint64>,
          Pointer<Uint64>,
          Pointer<Uint64>,
          Pointer<Uint64>,
        )
      >('GetProcessTimes');
  final io = allocate(sizeOf<_IoCounters>()).cast<_IoCounters>();
  final times = allocate(sizeOf<Uint64>() * 4).cast<Uint64>();
  if (io == nullptr || times == nullptr) {
    if (io != nullptr) release(io.cast());
    if (times != nullptr) release(times.cast());
    throw StateError('Process counter allocation failed.');
  }
  try {
    if (readIo(current, io) == 0 ||
        readTime(current, times, times + 1, times + 2, times + 3) == 0) {
      throw StateError('Process counters unavailable.');
    }
    return {
      'cpu_kernel_100ns': times[2],
      'cpu_user_100ns': times[3],
      'read_operations': io.ref.readOperations,
      'write_operations': io.ref.writeOperations,
      'read_bytes': io.ref.readBytes,
      'write_bytes': io.ref.writeBytes,
      'other_operations': io.ref.otherOperations,
      'other_bytes': io.ref.otherBytes,
    };
  } finally {
    release(io.cast());
    release(times.cast());
  }
}
