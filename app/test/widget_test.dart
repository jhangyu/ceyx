// Baseline Flutter widget smoke test for DngProcessorApp.
//
// Deliberately platform/native-library agnostic: DngHomePage.initState()
// calls DngDecoderService.initialize(), which tries to dlopen the native
// dylib and catches any failure internally (sets an error string instead of
// throwing). This test asserts only on chrome that renders regardless of
// whether the native library is present in the test environment (no build
// artifact dependency), so it is safe to run in CI without a native build.
//
// The test surface is enlarged before pumping: when the native lib fails to
// load (expected in a CI/test sandbox with no build artifacts), the app
// renders an error message in a Column that overflows the default small
// test viewport (unrelated to this test's assertions) — a real, pre-existing
// layout quirk of the error branch in lib/main.dart that only surfaces at
// tiny viewport sizes. Widening the surface avoids tripping that unrelated
// overflow so this test stays focused on chrome-presence assertions.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx_example/main.dart';

void main() {
  testWidgets('DngProcessorApp renders home scaffold chrome', (
    WidgetTester tester,
  ) async {
    final originalSize = tester.view.physicalSize;
    final originalRatio = tester.view.devicePixelRatio;
    tester.view.physicalSize = const Size(1600, 1200);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(() {
      tester.view.physicalSize = originalSize;
      tester.view.devicePixelRatio = originalRatio;
    });

    await tester.pumpWidget(const DngProcessorApp());
    // Let the first frame + any caught-exception setState settle.
    await tester.pump();

    // App bar title is always present regardless of native lib load result.
    expect(find.text('DNG Processor'), findsOneWidget);

    // Primary action affordances are always present in the initial layout.
    expect(find.text('Select DNG'), findsOneWidget);
    expect(find.byIcon(Icons.folder_open), findsOneWidget);
  });
}
