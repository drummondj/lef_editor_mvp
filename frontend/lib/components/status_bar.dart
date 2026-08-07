import 'package:flutter/material.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

class StatusBar extends StatelessWidget {
  const StatusBar({super.key});

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: Row(
            spacing: 8,
            mainAxisSize: .min,
            children: [
              SelectableText(
                "X: ${provider.snappedMousePosition.dx.toStringAsFixed(3)} Y: ${provider.snappedMousePosition.dy.toStringAsFixed(3)}",
              ),
              SelectableText("Selected: ${provider.selectedCount}"),
            ],
          ),
        );
      },
    );
  }
}
