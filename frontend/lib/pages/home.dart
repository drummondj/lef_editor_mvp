import 'package:flutter/material.dart';
import 'package:lef_editor/components/layer_manager.dart';
import 'package:lef_editor/components/layout_editor.dart';
import 'package:lef_editor/components/main_menu.dart';
import 'package:lef_editor/components/right_sidebar.dart';
import 'package:lef_editor/components/status_bar.dart';

class Home extends StatelessWidget {
  const Home({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(8.0),
          child: Row(
            crossAxisAlignment: .start,
            children: [
              MainMenu(),
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.all(8.0),
                  child: Column(
                    children: [
                      Expanded(flex: 5, child: LayoutEditor()),
                      StatusBar(),
                    ],
                  ),
                ),
              ),
              SizedBox(width: 400, child: RightSidebar()),
            ],
          ),
        ),
      ),
    );
  }
}
