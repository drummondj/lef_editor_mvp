import 'package:flutter/material.dart';
import 'package:lef_editor/components/layout_editor.dart';
import 'package:lef_editor/components/main_menu.dart';
import 'package:lef_editor/components/right_sidebar/right_sidebar.dart';
import 'package:lef_editor/components/status_bar.dart';
import 'package:lef_editor/components/terminal/terminal.dart';
import 'package:lef_editor/components/widget_card.dart';

class Home extends StatefulWidget {
  const Home({super.key});

  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  final double leftWidth = 300;
  final double rightWidth = 110;

  @override
  Widget build(BuildContext context) {
    final size = MediaQuery.of(context).size;
    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(8.0),
          child: Stack(
            children: [
              Positioned(
                left: leftWidth,
                right: rightWidth,
                height: 300,
                bottom: 100,
                child: WidgetCard(child: Terminal()),
              ),
              Positioned(
                left: leftWidth,
                right: rightWidth,
                bottom: 0,
                height: 100,
                child: WidgetCard(child: StatusBar()),
              ),
              Positioned(
                top: 10,
                left: leftWidth,
                right: rightWidth,
                bottom: 400,
                child: LayoutEditor(),
              ),
              Positioned(
                top: 0,
                left: 0,
                bottom: 0,
                width: leftWidth,
                child: WidgetCard(child: MainMenu()),
              ),
              Positioned(top: 0, bottom: 0, right: 0, child: RightSidebar()),
            ],
          ),
        ),
      ),
    );
  }
}
