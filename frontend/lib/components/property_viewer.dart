import 'package:flutter/material.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:lef_editor_plugin/lef_editor_plugin.dart';
import 'package:provider/provider.dart';

class PropertyViewer extends StatefulWidget {
  const PropertyViewer({super.key});

  @override
  State<PropertyViewer> createState() => _PropertyViewerState();
}

class _PropertyViewerState extends State<PropertyViewer> {
  int _currentIndex = 0;

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final selectedObjects = provider.selectedObjects;
        if (_currentIndex > provider.selectedObjects.length) {
          _currentIndex = 0;
        }
        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: SingleChildScrollView(
            child: Column(
              mainAxisSize: .min,
              children: selectedObjects.isEmpty
                  ? [Text("No selection")]
                  : [
                      Row(
                        mainAxisAlignment: .end,
                        children: [
                          IconButton(
                            icon: Icon(Icons.chevron_left),
                            onPressed: _currentIndex <= 0
                                ? null
                                : () {
                                    setState(() {
                                      _currentIndex--;
                                    });
                                  },
                          ),
                          Text(
                            "${_currentIndex + 1} / ${selectedObjects.length}",
                          ),
                          IconButton(
                            icon: Icon(Icons.chevron_right),
                            onPressed:
                                _currentIndex >= selectedObjects.length - 1
                                ? null
                                : () {
                                    setState(() {
                                      _currentIndex++;
                                    });
                                  },
                          ),
                        ],
                      ),
                      SelectedObjectTable(
                        object: selectedObjects[_currentIndex],
                      ),
                    ],
            ),
          ),
        );
      },
    );
  }
}

class SelectedObjectTable extends StatelessWidget {
  final LeSelectedObjectInfo object;

  const SelectedObjectTable({super.key, required this.object});

  String get _kindLabel => switch (object.kind) {
    LeSelectionKind.LE_SELECTION_KIND_TERMINAL => "Terminal",
    LeSelectionKind.LE_SELECTION_KIND_OBSTRUCTION => "Obstruction",
  };

  String _formatValue(LeSelectedProperty property) => switch (property.type) {
    LePropertyType.LE_PROPERTY_TYPE_DOUBLE =>
      (property.value as double).toStringAsFixed(3),
    LePropertyType.LE_PROPERTY_TYPE_STRING ||
    LePropertyType.LE_PROPERTY_TYPE_INT => property.value.toString(),
  };

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: .min,
      crossAxisAlignment: .center,
      children: [
        Padding(
          padding: const EdgeInsets.symmetric(vertical: 4.0),
          child: Center(
            child: Text(
              _kindLabel,
              style: Theme.of(context).textTheme.titleMedium,
            ),
          ),
        ),
        DataTable(
          dataRowMaxHeight: 24,
          dataRowMinHeight: 16,
          dividerThickness: 0.0,
          headingRowHeight: 0.0,
          decoration: BoxDecoration(),
          columns: [
            DataColumn(label: Text("Property")),
            DataColumn(label: Text("Value")),
          ],
          rows: [
            for (final property in object.properties)
              DataRow(
                cells: [
                  DataCell(SelectableText(property.name)),
                  DataCell(SelectableText(_formatValue(property))),
                ],
              ),
          ],
        ),
      ],
    );
  }
}
