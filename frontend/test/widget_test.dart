import 'package:flutter_test/flutter_test.dart';

import 'package:lef_editor/main.dart';

void main() {
  testWidgets('Home page shows the Open LEF button', (WidgetTester tester) async {
    await tester.pumpWidget(const MyApp());

    expect(find.text('Open LEF ...'), findsOneWidget);
  });
}
