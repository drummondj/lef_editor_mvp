import 'package:flutter/material.dart';
import 'package:lef_editor/components/layer_manager.dart';
import 'package:lef_editor/components/property_viewer.dart';
import 'package:lef_editor/components/widget_card.dart';

class RightSidebar extends StatefulWidget {
  const RightSidebar({super.key});

  @override
  State<RightSidebar> createState() => _RightSidebarState();
}

class SidebarItemData {
  final String title;
  final Widget child;
  final IconData iconData;
  SidebarItemData({
    required this.title,
    required this.child,
    required this.iconData,
  });
}

class SidebarItem extends StatelessWidget {
  final SidebarItemData item;
  final GestureTapCallback onTap;
  final bool selected;
  const SidebarItem({
    super.key,
    required this.item,
    required this.onTap,
    required this.selected,
  });

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8.0),
      child: GestureDetector(
        onTap: onTap,
        child: Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              Icon(
                item.iconData,
                size: 32,
                color: selected ? Colors.white : Colors.grey,
              ),
              Text(
                item.title,
                style: Theme.of(context).textTheme.labelMedium?.copyWith(
                  color: selected ? Colors.white : Colors.grey,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _RightSidebarState extends State<RightSidebar> {
  final _items = [
    SidebarItemData(
      title: "Layers",
      iconData: Icons.layers_outlined,
      child: LayerManager(),
    ),
    SidebarItemData(
      title: "Properties",
      iconData: Icons.list_alt_outlined,
      child: PropertyViewer(),
    ),
  ];

  SidebarItemData? _selected;

  void _select(SidebarItemData item) {
    setState(() {
      if (_selected == item) {
        _selected = null;
      } else {
        _selected = item;
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    return Row(
      crossAxisAlignment: .start,
      children: [
        if (_selected != null) WidgetCard(child: _selected!.child),
        WidgetCard(
          child: Column(
            spacing: 16,
            mainAxisAlignment: .start,

            children: _items
                .map(
                  (item) => SidebarItem(
                    item: item,
                    onTap: () => _select(item),
                    selected: _selected == item,
                  ),
                )
                .toList(),
          ),
        ),
      ],
    );
  }
}
