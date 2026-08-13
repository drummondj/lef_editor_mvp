import 'package:docking/docking.dart';
import 'package:flutter/material.dart';
import 'package:lef_editor/components/layer_manager.dart';
import 'package:lef_editor/components/layout_editor.dart';
import 'package:lef_editor/components/main_menu.dart';
import 'package:lef_editor/components/property_viewer.dart';
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
    DockingLayout layout = DockingLayout(
      root: DockingRow([
        DockingItem(
          name: 'Main Menu',
          widget: WidgetCard(child: MainMenu()),
          closable: false,
          size: 300,
          keepAlive: true,
        ),
        DockingColumn([
          DockingItem(
            name: 'Layout',
            widget: WidgetCard(
              child: Column(
                children: [
                  Expanded(child: LayoutEditor()),
                  StatusBar(),
                ],
              ),
            ),
            closable: false,
            keepAlive: true,
          ),
          DockingItem(
            name: 'Console',
            widget: WidgetCard(child: Terminal()),
            closable: false,
            keepAlive: true,
            size: 300,
          ),
        ]),
        DockingTabs(size: 400, [
          DockingItem(
            name: 'Layers',
            widget: WidgetCard(child: LayerManager()),
            closable: false,
            keepAlive: true,
          ),
          DockingItem(
            name: 'Properties',
            widget: WidgetCard(child: PropertyViewer()),
            closable: false,
            keepAlive: true,
          ),
        ]),
      ]),
    );
    Docking docking = Docking(layout: layout);
    MultiSplitViewTheme theme = MultiSplitViewTheme(
      data: MultiSplitViewThemeData(
        dividerThickness: 16,
        dividerPainter: DividerPainters.grooved2(
          backgroundColor: Theme.of(context).colorScheme.surfaceDim,
          color: Theme.of(context).colorScheme.secondary,
          highlightedColor: Theme.of(context).colorScheme.primary,
        ),
      ),
      child: TabbedViewTheme(
        data: TabbedViewThemeData.dark()
          ..tab.selectedStatus.innerBottomBorder = BorderSide(
            color: Theme.of(context).colorScheme.secondary,
            width: 3,
          )
          ..tabsArea.color = Theme.of(context).colorScheme.surfaceDim
          ..tab.selectedStatus.decoration = BoxDecoration(
            color: Theme.of(context).colorScheme.surfaceContainerHighest,
          )
          ..tab.decoration = BoxDecoration(
            color: Theme.of(context).colorScheme.surfaceDim,
          )
          ..contentArea.decoration = BoxDecoration(
            border: BoxBorder.all(
              color: Theme.of(context).colorScheme.surfaceContainerHighest,
              width: 3,
            ),
          )
          ..contentArea.padding = EdgeInsets.all(2),
        child: docking,
      ),
    );
    return Scaffold(
      backgroundColor: Theme.of(context).colorScheme.surfaceDim,
      body: SafeArea(
        child: Padding(padding: const EdgeInsets.all(16), child: theme),
      ),
    );
  }
}
