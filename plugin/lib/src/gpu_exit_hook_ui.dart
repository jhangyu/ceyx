import 'dart:ui' show AppExitResponse;

import 'package:flutter/widgets.dart' show AppLifecycleListener, WidgetsBinding;

AppLifecycleListener? _listener;

/// Installs the single exit-request listener. Returns false while no
/// [WidgetsBinding] exists yet, so the caller retries on its next call.
/// Always answers [AppExitResponse.exit]: this hook only releases, it never
/// cancels a close.
bool installExitHook(Future<void> Function() onExit) {
  if (_listener != null) return true;
  try {
    WidgetsBinding.instance;
  } catch (_) {
    return false;
  }
  _listener = AppLifecycleListener(
    onExitRequested: () async {
      await onExit();
      return AppExitResponse.exit;
    },
  );
  return true;
}
