import 'package:flutter/material.dart';
import 'package:lef_editor/components/layer_manager.dart';
import 'package:lef_editor/components/property_viewer.dart';

class RightSidebar extends StatefulWidget {
  const RightSidebar({super.key});

  @override
  State<RightSidebar> createState() => _RightSidebarState();
}

class _RightSidebarState extends State<RightSidebar> {
  static const _tabs = [LayerManager(), PropertyViewer()];

  @override
  Widget build(BuildContext context) {
    return DefaultTabController(
      initialIndex: 0,
      length: _tabs.length,
      child: Column(
        children: [
          TabBar(tabs: [Tab(text: "Layers"), Tab(text: "Properties")]),
          Expanded(
            child: Builder(
              builder: (context) {
                final controller = DefaultTabController.of(context);
                return AnimatedBuilder(
                  animation: controller,
                  builder: (context, _) => _tabs[controller.index],
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}
