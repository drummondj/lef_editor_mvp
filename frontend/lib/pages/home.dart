import 'package:flutter/material.dart';
import 'package:lef_editor/components/layout_editor.dart';
import 'package:lef_editor/components/main_menu.dart';
import 'package:lef_editor/components/message_console.dart';
import 'package:lef_editor/components/right_sidebar/right_sidebar_alt.dart';
import 'package:lef_editor/components/tcl_console.dart';
import 'package:lef_editor/components/terminal/terminal.dart';
import 'package:lef_editor/components/widget_card.dart';

class Home extends StatefulWidget {
  const Home({super.key});

  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  final double leftWidth = 300;
  final double rightWidth = 100;

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
                right: 10,
                top: size.height - 300,
                bottom: 10,
                child: WidgetCard(child: Terminal()),
              ),
              Positioned(
                top: 10,
                left: leftWidth,
                right: 10,
                bottom: 300,
                child: LayoutEditor(),
              ),
              Positioned(
                top: 0,
                left: 0,
                bottom: 0,
                width: leftWidth,
                child: WidgetCard(child: MainMenu()),
              ),
              // Positioned(
              //   bottom: 0,
              //   left: leftWidth,
              //   right: rightWidth,
              //   child: WidgetCard(
              //     child: Column(
              //       mainAxisSize: MainAxisSize.min,
              //       children: [MessageConsole(), TclConsole()],
              //     ),
              //   ),
              // ),
              // Positioned(top: 0, bottom: 0, right: 0, child: RightSidebarAlt()),
            ],
          ),
        ),
      ),
    );
  }
}
