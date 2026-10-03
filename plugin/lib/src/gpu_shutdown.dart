import 'dart:async';

import 'package:meta/meta.dart';

import 'dng_bindings.dart';
import 'gpu_exit_hook_stub.dart'
    if (dart.library.ui) 'gpu_exit_hook_ui.dart'
    as exit_hook;

/// Thrown by a direct (non-pool) decode/encode entry point once the app is
/// closing: the native GPU context is released at exit and no work may start
/// after that.
class CeyxShutdownException implements Exception {
  CeyxShutdownException(this.entry);

  final String entry;

  @override
  String toString() => 'CeyxShutdownException: $entry refused, app is closing';
}

/// Exit-time release of the native GPU context (IC9
/// `ceyx_native_release_gpu`), so Halide's Vulkan static destructor finds
/// nothing to tear down inside an already-unloaded driver.
///
/// One mechanism on every platform: an application exit-request listener
/// installed lazily from the first [CeyxDecodePool]. Where the OS never issues
/// an exit request (Android, iOS) it is simply never invoked.
///
/// Order on an exit request: set the closing latch (every entry point now
/// refuses work), wait up to [quiescenceBound] for every registered pool to
/// drain, call the export once, answer "exit". Not quiescent in time: the
/// release is skipped and logged, never made under a live decode.
class CeyxGpuShutdown {
  CeyxGpuShutdown._();

  static const Duration quiescenceBound = Duration(seconds: 2);

  static void Function(String line) logger = _defaultLog;

  static void _defaultLog(String line) {
    // ignore: avoid_print
    print(line);
  }

  @visibleForTesting
  static void Function()? debugReleaseOverride;

  static final List<Future<bool> Function(Duration)> _waiters = [];
  static bool _closing = false;
  static bool _released = false;
  static bool _hookInstalled = false;

  static bool get isClosing => _closing;
  static bool get isReleased => _released;

  /// Direct entry points call this first; the pool uses its own typed error.
  static void guardWork(String entry) {
    if (_closing) throw CeyxShutdownException(entry);
  }

  /// Registers a pool's "wait until quiescent, true if reached" probe and
  /// makes sure the exit hook is installed.
  static void register(Future<bool> Function(Duration bound) waitQuiescent) {
    _waiters.add(waitQuiescent);
    ensureInstalled();
  }

  static void unregister(Future<bool> Function(Duration bound) waitQuiescent) {
    _waiters.remove(waitQuiescent);
  }

  static void ensureInstalled() {
    if (_hookInstalled) return;
    _hookInstalled = exit_hook.installExitHook(prepareForExit);
  }

  @visibleForTesting
  static Future<void> prepareForExit() async {
    if (_closing) return;
    _closing = true;
    final probes = List<Future<bool> Function(Duration)>.of(_waiters);
    final results = await Future.wait(
      probes.map((wait) => wait(quiescenceBound)),
    );
    if (results.any((quiescent) => !quiescent)) {
      logger('[CeyxExit] release=skipped reason=not-quiescent');
      return;
    }
    _release();
  }

  static void _release() {
    final CeyxNativeReleaseGpuDart? fn;
    try {
      fn =
          debugReleaseOverride ?? DngNativeBindings.load().ceyxNativeReleaseGpu;
    } catch (error) {
      logger('[CeyxExit] release=skipped reason=library-unavailable ($error)');
      return;
    }
    if (fn == null) {
      logger('[CeyxExit] release=skipped reason=symbol-absent');
      return;
    }
    _released = true;
    try {
      fn();
      logger('[CeyxExit] release=done');
    } catch (error) {
      logger('[CeyxExit] release=failed reason=$error');
    }
  }

  @visibleForTesting
  static void debugReset() {
    _closing = false;
    _released = false;
    _waiters.clear();
    debugReleaseOverride = null;
    logger = _defaultLog;
  }
}
