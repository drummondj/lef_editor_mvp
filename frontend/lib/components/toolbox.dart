import 'package:flutter/material.dart';
import 'package:hugeicons/hugeicons.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:lef_editor_plugin/lef_editor_plugin.dart';
import 'package:provider/provider.dart';

class ToolboxButton extends StatelessWidget {
  final String text;
  final String? shortcutKey;
  final dynamic icon;
  final VoidCallback onPressed;
  final bool selected;
  final bool large;

  const ToolboxButton({
    super.key,
    required this.text,
    this.shortcutKey,
    required this.icon,
    required this.onPressed,
    required this.selected,
    this.large = true,
  });

  @override
  Widget build(BuildContext context) {
    final colorScheme = Theme.of(context).colorScheme;
    final forgroundColor = selected
        ? colorScheme.tertiary
        : colorScheme.primary;
    final strokeWidth = selected ? 1.5 : 1.0;
    final FontWeight fontWeight = selected ? .bold : .normal;
    final finalText = shortcutKey != null ? "$text\n($shortcutKey)" : "$text\n";

    return Container(
      decoration: selected
          ? BoxDecoration(
              border: .fromLTRB(
                left: BorderSide(color: forgroundColor, width: 4.0),
              ),
              gradient: LinearGradient(
                colors: [forgroundColor.withAlpha(25), Colors.transparent],
              ),
            )
          : null,
      child: Padding(
        padding: EdgeInsets.all(large ? 8.0 : 4),
        child: Tooltip(
          message: large ? "" : finalText,
          child: TextButton(
            onPressed: selected ? null : onPressed,
            child: Column(
              children: [
                HugeIcon(
                  icon: icon,
                  color: forgroundColor,
                  size: large ? 48 : 36,
                  strokeWidth: strokeWidth,
                ),
                if (large)
                  Text(
                    finalText,
                    textAlign: .center,
                    style: Theme.of(context).textTheme.labelMedium?.copyWith(
                      color: forgroundColor,
                      fontWeight: fontWeight,
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class Toolbox extends StatefulWidget {
  const Toolbox({super.key});

  @override
  State<Toolbox> createState() => _ToolboxState();
}

class _ToolboxState extends State<Toolbox> {
  List<Widget> _getModeMenuItems(BuildContext context, LeMode mode) {
    final provider = context.read<LeProvider>();
    return switch (mode) {
      .LE_MODE_SELECT => [
        ToolboxButton(
          icon: HugeIcons.strokeRoundedBoundingBox,
          text: 'Select all',
          shortcutKey: "ctrl-a",
          onPressed: () => provider.selectAll(),
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedCancel02,
          text: 'De-select all',
          shortcutKey: "ctrl-d",
          onPressed: () => provider.deselectAll(),
          selected: false,
          large: false,
        ),
      ],
      .LE_MODE_EDIT => [
        ToolboxButton(
          icon: HugeIcons.strokeRoundedArrowAllDirection,
          text: 'Move',
          shortcutKey: "ctrl-m",
          onPressed: () => provider.armMove(),
          selected: provider.moveArmed,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedMaximizeScreen,
          text: 'Resize',
          shortcutKey: "ctrl-r",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedRotateCrop,
          text: 'Rotate',
          shortcutKey: "ctrl-o",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedAlignBottom,
          text: 'Align Bottom',
          shortcutKey: "ctrl-b",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedAlignVerticalCenter,
          text: 'Align Vertcial Center',
          shortcutKey: "ctrl-v",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedAlignTop,
          text: 'Align Top',
          shortcutKey: "ctrl-t",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedAlignLeft,
          text: 'Align Left',
          shortcutKey: "ctrl-l",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedAlignHorizontalCenter,
          text: 'Align Horizontal Center',
          shortcutKey: "ctrl-h",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedAlignRight,
          text: 'Align Right',
          shortcutKey: "ctrl-w",
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedDelete04,
          shortcutKey: "ctrl-d",
          text: 'Delete',
          onPressed: () => {},
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedUndo03,
          shortcutKey: "ctrl-z",
          text: 'Undo',
          onPressed: () => provider.undo(),
          selected: false,
          large: false,
        ),
        ToolboxButton(
          icon: HugeIcons.strokeRoundedRedo03,
          shortcutKey: "shift-ctrl-z",
          text: 'Redo',
          onPressed: () => provider.redo(),
          selected: false,
          large: false,
        ),
      ],
      .LE_MODE_RULER => [
        ToolboxButton(
          icon: HugeIcons.strokeRoundedCancel02,
          text: 'Clear rulers',
          onPressed: () => provider.clearRulers(),
          selected: false,
          large: false,
        ),
      ],
    };
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final mode = provider.mode;
        return Padding(
          padding: const .all(8),
          child: Row(
            crossAxisAlignment: .start,
            children: [
              Column(
                spacing: 16,
                children: [
                  ToolboxButton(
                    text: "Select",
                    shortcutKey: "s",
                    icon: HugeIcons.strokeRoundedCursorRectangleSelection01,
                    onPressed: () => provider.setMode(LeMode.LE_MODE_SELECT),
                    selected: mode == LeMode.LE_MODE_SELECT,
                  ),
                  ToolboxButton(
                    text: "Edit",
                    shortcutKey: "e",
                    icon: HugeIcons.strokeRoundedCursorEdit01,
                    onPressed: () => provider.setMode(LeMode.LE_MODE_EDIT),
                    selected: mode == LeMode.LE_MODE_EDIT,
                  ),
                  ToolboxButton(
                    text: "Ruler",
                    shortcutKey: "r",
                    icon: HugeIcons.strokeRoundedRuler,
                    onPressed: () => provider.setMode(LeMode.LE_MODE_RULER),
                    selected: mode == LeMode.LE_MODE_RULER,
                  ),
                ],
              ),
              VerticalDivider(
                width: 20,
                thickness: 1,
                color: Theme.of(context).colorScheme.surfaceContainerHighest,
              ),
              Expanded(
                child: Card(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(vertical: 16.0),
                    child: Wrap(
                      spacing: 4,
                      runSpacing: 4,
                      direction: .horizontal,
                      children: [..._getModeMenuItems(context, mode)],
                    ),
                  ),
                ),
              ),
            ],
          ),
        );
      },
    );
  }
}
