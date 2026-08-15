import 'package:flutter/material.dart';
import 'package:hugeicons/hugeicons.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:lef_editor_plugin/lef_editor_plugin.dart';
import 'package:provider/provider.dart';

class ToolboxButton extends StatelessWidget {
  final String text;
  final dynamic icon;
  final VoidCallback onPressed;
  final bool selected;

  const ToolboxButton({
    super.key,
    required this.text,
    required this.icon,
    required this.onPressed,
    required this.selected,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final color = selected ? colorScheme.tertiary : colorScheme.primary;
    return TextButton(
      onPressed: selected ? null : onPressed,
      child: Column(
        children: [
          HugeIcon(icon: icon, color: color, size: 32, strokeWidth: 0.75),
          Text(
            text,
            style: Theme.of(
              context,
            ).textTheme.labelMedium?.copyWith(color: color),
          ),
        ],
      ),
    );
  }
}

class Toolbox extends StatelessWidget {
  const Toolbox({super.key});

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final mode = provider.mode;
        return Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8.0, vertical: 32),
          child: Wrap(
            spacing: 32,
            runSpacing: 32,
            alignment: .center,
            children: [
              ToolboxButton(
                text: "Select (s)",
                icon: HugeIcons.strokeRoundedCursorRectangleSelection01,
                onPressed: () => provider.setMode(LeMode.LE_MODE_SELECT),
                selected: mode == LeMode.LE_MODE_SELECT,
              ),
              ToolboxButton(
                text: "Edit (e)",
                icon: HugeIcons.strokeRoundedCursorEdit01,
                onPressed: () => provider.setMode(LeMode.LE_MODE_EDIT),
                selected: mode == LeMode.LE_MODE_EDIT,
              ),
              ToolboxButton(
                text: "Ruler (r)",
                icon: HugeIcons.strokeRoundedRuler,
                onPressed: () => provider.setMode(LeMode.LE_MODE_RULER),
                selected: mode == LeMode.LE_MODE_RULER,
              ),
            ],
          ),
        );
      },
    );
  }
}
