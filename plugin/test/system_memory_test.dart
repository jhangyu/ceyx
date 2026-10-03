import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  tearDown(() => debugCeyxPhysicalMemoryBytesOverride = null);

  test('TC-1457 one physical-memory source: a non-positive native answer '
      'is "no reading", a positive one passes through', () {
    debugCeyxPhysicalMemoryBytesOverride = () => 0;
    expect(ceyxPhysicalMemoryBytes(), isNull);
    debugCeyxPhysicalMemoryBytesOverride = () => 34359738368;
    expect(ceyxPhysicalMemoryBytes(), 34359738368);
  });
}
