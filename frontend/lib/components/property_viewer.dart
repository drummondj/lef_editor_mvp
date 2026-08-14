import 'package:flutter/foundation.dart';
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
  late final TextEditingController _filterController;
  String _filter = '';
  bool _showHiddenProperties = false;

  // Database-hierarchy navigation (UPDATES.md 7.2's Property Viewer
  // redesign) - purely local widget state, entirely independent of
  // LeProvider/canvas selection. `_breadcrumb.last` is the ref currently
  // displayed; clicking a parent/child link only ever pushes/truncates
  // this list via setState (see _navigateTo/_navigateToBreadcrumbIndex) -
  // it never calls into anything that could mutate canvas selection.
  // Reset to `[selectedObjects[_currentIndex]]` only when the outer pager
  // index or the underlying selection itself changes (detected in
  // _syncBreadcrumb, called once per build) - never on a link click.
  List<LeObjectRef> _breadcrumb = const [];
  List<LeObjectRef> _lastSelectedObjects = const [];
  int _lastCurrentIndex = -1;

  @override
  void initState() {
    super.initState();
    _filterController = TextEditingController();
    _filterController.addListener(() {
      setState(() {
        _filter = _filterController.text;
      });
    });
  }

  @override
  void dispose() {
    _filterController.dispose();
    super.dispose();
  }

  void _syncBreadcrumb(List<LeObjectRef> selectedObjects) {
    if (_currentIndex >= selectedObjects.length) {
      _currentIndex = 0;
    }
    final selectionChanged = !listEquals(
      selectedObjects,
      _lastSelectedObjects,
    );
    final indexChanged = _currentIndex != _lastCurrentIndex;
    _lastSelectedObjects = selectedObjects;
    _lastCurrentIndex = _currentIndex;

    if (selectedObjects.isEmpty) {
      _breadcrumb = const [];
      return;
    }
    if (selectionChanged || indexChanged || _breadcrumb.isEmpty) {
      _breadcrumb = [selectedObjects[_currentIndex]];
    }
  }

  void _navigateTo(LeObjectRef ref) {
    setState(() {
      _breadcrumb = [..._breadcrumb, ref];
    });
  }

  void _navigateToBreadcrumbIndex(int index) {
    setState(() {
      _breadcrumb = _breadcrumb.sublist(0, index + 1);
    });
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        final selectedObjects = provider.selectedObjects;
        _syncBreadcrumb(selectedObjects);

        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: SingleChildScrollView(
            child: Column(
              mainAxisSize: .min,
              children: selectedObjects.isEmpty
                  ? [Text("No selection")]
                  : [
                      Row(
                        children: [
                          Checkbox(
                            value: _showHiddenProperties,
                            visualDensity: VisualDensity.compact,
                            materialTapTargetSize:
                                MaterialTapTargetSize.shrinkWrap,
                            onChanged: (bool? value) {
                              setState(() {
                                _showHiddenProperties = value ?? false;
                              });
                            },
                          ),
                          const SizedBox(width: 4),
                          Text(
                            'Show hidden properties',
                            style: Theme.of(context).textTheme.bodySmall,
                          ),
                          Spacer(),
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
                      Padding(
                        padding: const EdgeInsets.symmetric(vertical: 4.0),
                        child: SearchBar(
                          controller: _filterController,
                          hintText: 'Filter properties',
                          constraints: const BoxConstraints(maxHeight: 32),
                          padding: const WidgetStatePropertyAll(
                            EdgeInsets.symmetric(horizontal: 4),
                          ),
                          textStyle: WidgetStatePropertyAll(
                            Theme.of(context).textTheme.bodySmall,
                          ),
                          hintStyle: WidgetStatePropertyAll(
                            Theme.of(context).textTheme.bodySmall,
                          ),
                          leading: const Padding(
                            padding: EdgeInsets.only(left: 8),
                            child: Icon(Icons.filter_list, size: 16),
                          ),
                          trailing: [
                            IconButton(
                              icon: const Icon(Icons.clear, size: 16),
                              visualDensity: VisualDensity.compact,
                              onPressed: _filterController.clear,
                            ),
                          ],
                        ),
                      ),
                      _ObjectBreadcrumb(
                        breadcrumb: _breadcrumb,
                        onTapIndex: _navigateToBreadcrumbIndex,
                      ),
                      ObjectDetail(
                        ref: _breadcrumb.last,
                        filter: _filter,
                        showHiddenProperties: _showHiddenProperties,
                        onNavigate: _navigateTo,
                      ),
                    ],
            ),
          ),
        );
      },
    );
  }
}

/// A short, human-readable chip label for [ref] - its own "name" property
/// if it has one (Library/Design/Terminal), else `Kind #index`.
String _labelFor(LeProvider provider, LeObjectRef ref) {
  if (!ref.isValid) return "?";
  final properties = provider.objectProperties(ref);
  for (final property in properties) {
    if (property.name == "name") {
      return property.value.toString();
    }
  }
  return "${_kindLabel(ref.kind)} #${ref.index}";
}

String _kindLabel(LeObjectKind kind) => switch (kind) {
  LeObjectKind.LE_OBJECT_KIND_LIBRARY => "Library",
  LeObjectKind.LE_OBJECT_KIND_DESIGN => "Design",
  LeObjectKind.LE_OBJECT_KIND_ABSTRACT => "Abstract",
  LeObjectKind.LE_OBJECT_KIND_TERMINAL => "Terminal",
  LeObjectKind.LE_OBJECT_KIND_TERMINAL_PORT => "TerminalPort",
  LeObjectKind.LE_OBJECT_KIND_OBSTRUCTION => "Obstruction",
  LeObjectKind.LE_OBJECT_KIND_SHAPE => "Shape",
};

/// Breadcrumb chip row (UPDATES.md 7.2) - one chip per entry in
/// [breadcrumb], the last one visually current; tapping an earlier chip
/// truncates back to it (see _PropertyViewerState._navigateToBreadcrumbIndex).
/// Purely a rendering/tap-forwarding widget - owns no navigation state
/// itself.
class _ObjectBreadcrumb extends StatelessWidget {
  final List<LeObjectRef> breadcrumb;
  final ValueChanged<int> onTapIndex;

  const _ObjectBreadcrumb({required this.breadcrumb, required this.onTapIndex});

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<LeProvider>();
    final lastIndex = breadcrumb.length - 1;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4.0),
      child: Wrap(
        crossAxisAlignment: WrapCrossAlignment.center,
        children: [
          for (final (index, ref) in breadcrumb.indexed) ...[
            if (index > 0)
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 2),
                child: Icon(Icons.chevron_right, size: 14),
              ),
            index == lastIndex
                ? Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 4),
                    child: Text(
                      _labelFor(provider, ref),
                      style: Theme.of(context).textTheme.bodyMedium
                          ?.copyWith(fontWeight: FontWeight.bold),
                    ),
                  )
                : InkWell(
                    onTap: () => onTapIndex(index),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                        horizontal: 4,
                        vertical: 2,
                      ),
                      child: Text(
                        _labelFor(provider, ref),
                        style: Theme.of(context).textTheme.bodyMedium
                            ?.copyWith(
                              color: Theme.of(context).colorScheme.primary,
                              decoration: TextDecoration.underline,
                            ),
                      ),
                    ),
                  ),
          ],
        ],
      ),
    );
  }
}

/// The currently-navigated-to object's own property table (UPDATES.md
/// 7.2) plus its parent/child links - the same object hierarchy the
/// database (and TCL's get_properties) exposes. Clicking a link calls
/// [onNavigate] (forwarded up to _PropertyViewerState._navigateTo) -
/// purely local navigation, never a provider mutation.
class ObjectDetail extends StatelessWidget {
  final LeObjectRef ref;
  final String filter;
  final bool showHiddenProperties;
  final ValueChanged<LeObjectRef> onNavigate;

  const ObjectDetail({
    super.key,
    required this.ref,
    required this.filter,
    required this.showHiddenProperties,
    required this.onNavigate,
  });

  String _formatValue(LeSelectedProperty property) => switch (property.type) {
    LePropertyType.LE_PROPERTY_TYPE_DOUBLE =>
      (property.value as double).toStringAsFixed(3),
    LePropertyType.LE_PROPERTY_TYPE_STRING ||
    LePropertyType.LE_PROPERTY_TYPE_INT => property.value.toString(),
  };

  @override
  Widget build(BuildContext context) {
    final provider = context.watch<LeProvider>();
    final ColorScheme colorScheme = Theme.of(context).colorScheme;
    final String normalizedFilter = filter.trim().toLowerCase();
    final List<LeSelectedProperty> properties = provider.objectProperties(
      ref,
    );
    final Iterable<LeSelectedProperty> filteredProperties = properties
        .where(
          (property) =>
              showHiddenProperties || _formatValue(property).isNotEmpty,
        )
        .where(
          (property) =>
              normalizedFilter.isEmpty ||
              property.name.toLowerCase().contains(normalizedFilter),
        );

    final LeObjectRef parent = provider.objectParent(ref);
    final List<LeObjectRef> children = provider.objectChildren(ref);

    return Column(
      mainAxisSize: .min,
      crossAxisAlignment: .center,
      children: [
        Padding(
          padding: const EdgeInsets.symmetric(vertical: 4.0),
          child: Center(
            child: Text(
              _kindLabel(ref.kind),
              style: Theme.of(context).textTheme.titleMedium,
            ),
          ),
        ),
        // DataTable's dividerThickness only ever sets the row divider's
        // *width* - Flutter treats BorderSide(width: 0.0) as a special
        // "hairline" border that still renders as one physical pixel, not
        // as no border at all. The divider only disappears by making its
        // color transparent, which DataTable reads from the ambient
        // DividerTheme rather than exposing directly itself.
        DividerTheme(
          data: const DividerThemeData(color: Colors.transparent),
          child: DataTable(
            dividerThickness: 0.0,
            headingRowHeight: 0.0,
            decoration: BoxDecoration(border: Border.all(width: 0)),
            columns: [
              DataColumn(label: Text("Property")),
              // Without an explicit columnWidth, DataTable sizes every
              // column to its content (IntrinsicColumnWidth), so the whole
              // table shrinks instead of filling the available width.
              // FlexColumnWidth lets this one column absorb the rest.
              DataColumn(
                label: Text("Value"),
                columnWidth: const FlexColumnWidth(),
              ),
            ],
            rows: [
              for (final (index, property) in filteredProperties.indexed)
                DataRow(
                  color: WidgetStateProperty.all(
                    index.isEven ? colorScheme.surface : colorScheme.surfaceDim,
                  ),
                  cells: [
                    DataCell(SelectableText(property.name)),
                    DataCell(SelectableText(_formatValue(property))),
                  ],
                ),
            ],
          ),
        ),
        if (parent.isValid)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 4.0),
            child: Align(
              alignment: Alignment.centerLeft,
              child: InkWell(
                onTap: () => onNavigate(parent),
                child: Text.rich(
                  TextSpan(
                    children: [
                      TextSpan(
                        text: "Parent: ",
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                      TextSpan(
                        text: _labelFor(provider, parent),
                        style: Theme.of(context).textTheme.bodySmall
                            ?.copyWith(
                              color: colorScheme.primary,
                              decoration: TextDecoration.underline,
                            ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        if (children.isNotEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 4.0),
            child: Align(
              alignment: Alignment.centerLeft,
              child: Wrap(
                crossAxisAlignment: WrapCrossAlignment.center,
                children: [
                  Text(
                    "${_childLabel(children.first.kind)} (${children.length}): ",
                    style: Theme.of(context).textTheme.bodySmall,
                  ),
                  for (final child in children)
                    Padding(
                      padding: const EdgeInsets.only(right: 6),
                      child: InkWell(
                        onTap: () => onNavigate(child),
                        child: Text(
                          _labelFor(provider, child),
                          style: Theme.of(context).textTheme.bodySmall
                              ?.copyWith(
                                color: colorScheme.primary,
                                decoration: TextDecoration.underline,
                              ),
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ),
      ],
    );
  }

  String _childLabel(LeObjectKind kind) => switch (kind) {
    LeObjectKind.LE_OBJECT_KIND_TERMINAL_PORT => "Ports",
    LeObjectKind.LE_OBJECT_KIND_SHAPE => "Shapes",
    LeObjectKind.LE_OBJECT_KIND_LIBRARY => "Libraries",
    LeObjectKind.LE_OBJECT_KIND_DESIGN => "Designs",
    LeObjectKind.LE_OBJECT_KIND_ABSTRACT => "Abstracts",
    LeObjectKind.LE_OBJECT_KIND_TERMINAL => "Terminals",
    LeObjectKind.LE_OBJECT_KIND_OBSTRUCTION => "Obstructions",
  };
}
