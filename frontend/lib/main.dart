import 'package:flutter/material.dart';
import 'package:lef_editor/pages/home.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'LEF Editor',
      home: MultiProvider(
        providers: [ChangeNotifierProvider(create: (context) => LeProvider())],
        child: const Home(),
      ),
      debugShowCheckedModeBanner: false,
      themeMode: .dark,
      darkTheme: .dark(),
      theme: .light(),
    );
  }
}
