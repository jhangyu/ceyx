import 'dng_bindings.dart';

int? Function()? debugCeyxPhysicalMemoryBytesOverride;

DngNativeBindings? _bindings;
bool _attempted = false;

/// Total physical RAM from the ceyx native library -- the same function the
/// decoder's own recommendation reads -- or null when the library or symbol
/// is absent or the platform read failed. Synchronous; safe before runApp.
int? ceyxPhysicalMemoryBytes() {
  final override = debugCeyxPhysicalMemoryBytesOverride;
  final int? bytes;
  if (override != null) {
    bytes = override();
  } else {
    if (!_attempted) {
      _attempted = true;
      try {
        _bindings = DngNativeBindings.load();
      } catch (_) {
        _bindings = null;
      }
    }
    bytes = _bindings?.ceyxPhysicalMemoryBytes?.call();
  }
  return bytes == null || bytes <= 0 ? null : bytes;
}
