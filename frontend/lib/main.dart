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
    return MultiProvider(
      providers: [ChangeNotifierProvider(create: (context) => LeProvider())],
      child: MaterialApp(
        title: 'LEF Editor',
        home: const Home(),
        debugShowCheckedModeBanner: false,
        themeMode: .dark,
        darkTheme: ThemeData(
          fontFamily: 'JetBrains Mono',
          colorScheme: ColorScheme(
            brightness: .dark,
            primary: const Color.fromARGB(255, 220, 220, 220),
            onPrimary: Colors.black,
            secondary: Colors.orange,
            onSecondary: Colors.black,
            error: Colors.red,
            onError: Colors.black,
            surface: Color.fromARGB(255, 30, 30, 30),
            onSurface: Color.fromARGB(255, 220, 220, 220),
            surfaceDim: Color.fromARGB(255, 0, 0, 0),
            surfaceContainer: Color.fromARGB(255, 10, 10, 10),
          ),
        ),
        theme: ThemeData(brightness: .light, fontFamily: 'JetBrains Mono'),
      ),
    );
  }
}
