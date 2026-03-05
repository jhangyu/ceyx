import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor/main.dart';

void main() {
  testWidgets('DNG Processor app renders', (WidgetTester tester) async {
    await tester.pumpWidget(const DngProcessorApp());

    // Verify the app title is shown
    expect(find.text('DNG Processor'), findsOneWidget);

    // Verify the "Select DNG" button exists
    expect(find.text('Select DNG'), findsOneWidget);

    // Verify the empty state text
    expect(find.text('Select a DNG file to decode'), findsOneWidget);
  });
}
