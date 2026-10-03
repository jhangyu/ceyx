/// Plain-Dart consumers (no `dart:ui`) have no application exit request to
/// hook, so nothing is installed. Selected by the conditional import in
/// `gpu_shutdown.dart`; the Flutter build uses `gpu_exit_hook_ui.dart`.
bool installExitHook(Future<void> Function() onExit) => false;
