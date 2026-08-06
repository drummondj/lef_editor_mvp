import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:lef_editor/components/layer_manager.dart';
import 'package:lef_editor/components/library_browser.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

class Home extends StatefulWidget {
  const Home({super.key});

  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  Size? _lastViewportSize;
  Offset _currentMousePosition = Offset.zero;
  final FocusNode _textureFocusNode = FocusNode();

  @override
  void initState() {
    super.initState();
    context.read<LeProvider>().init();
    _textureFocusNode.skipTraversal = true;
  }

  void _handleViewportResize(BoxConstraints constraints) {
    final size = Size(constraints.maxWidth, constraints.maxHeight);
    if (size == _lastViewportSize) return;
    _lastViewportSize = size;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) context.read<LeProvider>().resize(size);
    });
  }

  Future<void> _openFilePicker() async {
    const typeGroup = XTypeGroup(label: 'LEF files', extensions: ['lef']);
    final files = await openFiles(acceptedTypeGroups: [typeGroup]);
    if (files.isEmpty || !mounted) return;
    final provider = context.read<LeProvider>();
    for (final file in files) {
      await provider.readLef(file.path);
    }
  }

  void _openLibraryBrowser() async {
    showDialog(
      context: context,
      builder: (context) {
        return Dialog(
          child: SizedBox(width: 500, height: 400, child: LibraryBrowser()),
        );
      },
    );
  }

  void _loadTestData() async {
    var provider = context.read<LeProvider>();
    await provider.readLef(
      "/Users/john/Projects/synthosilicon/layout_engine/test_data/Nangate45/Nangate45_tech.lef",
    );
    await provider.readLef(
      "/Users/john/Projects/synthosilicon/layout_engine/test_data/Nangate45/Nangate45_stdcell.lef",
    );
    await provider.readLef(
      "/Users/john/Projects/synthosilicon/layout_engine/test_data/Nangate45/fakeram45_1024x32.lef",
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Consumer<LeProvider>(
          builder: (context, provider, child) {
            return Padding(
              padding: const EdgeInsets.all(8.0),
              child: Row(
                children: [
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(8.0),
                      child: Column(
                        crossAxisAlignment: .start,
                        spacing: 8,
                        children: [
                          TextButton.icon(
                            icon: Icon(Icons.folder_open),
                            onPressed: _openFilePicker,
                            label: Text("Import LEF ..."),
                          ),
                          TextButton.icon(
                            icon: Icon(Icons.list),
                            onPressed: _openLibraryBrowser,
                            label: Text("Library Browser ..."),
                          ),
                          TextButton.icon(
                            icon: Icon(Icons.my_library_books_sharp),
                            onPressed: _loadTestData,
                            label: Text("Load test data"),
                          ),
                        ],
                      ),
                    ),
                  ),
                  Expanded(
                    flex: 5,
                    child: LayoutBuilder(
                      builder: (context, constraints) {
                        _handleViewportResize(constraints);
                        return MouseRegion(
                          onHover: (event) {
                            setState(() {
                              _currentMousePosition = event.localPosition;
                            });
                          },
                          onEnter: (event) {
                            _textureFocusNode.requestFocus();
                          },
                          child: Focus(
                            focusNode: _textureFocusNode,
                            onKeyEvent: (node, value) {
                              if (value is KeyDownEvent ||
                                  value is KeyRepeatEvent) {
                                if (value.character == "z") {
                                  provider.zoomIn(_currentMousePosition);
                                  return KeyEventResult.handled;
                                } else if (value.character == "Z") {
                                  provider.zoomOut(_currentMousePosition);
                                  return KeyEventResult.handled;
                                } else if (value.character == "f") {
                                  provider.fit();
                                  return KeyEventResult.handled;
                                } else if (value.physicalKey == .arrowLeft) {
                                  provider.panLeft();
                                  return KeyEventResult.handled;
                                } else if (value.physicalKey == .arrowRight) {
                                  provider.panRight();
                                  return KeyEventResult.handled;
                                } else if (value.physicalKey == .arrowDown) {
                                  provider.panDown();
                                  return KeyEventResult.handled;
                                } else if (value.physicalKey == .arrowUp) {
                                  provider.panUp();
                                  return KeyEventResult.handled;
                                }
                              }
                              return KeyEventResult.ignored;
                            },
                            child: SizedBox(
                              width: constraints.maxWidth,
                              height: constraints.maxHeight,
                              child: provider.texture != null
                                  ? Texture(
                                      textureId: provider.texture!.textureId,
                                    )
                                  : null,
                            ),
                          ),
                        );
                      },
                    ),
                  ),
                  LayerManager(),
                ],
              ),
            );
          },
        ),
      ),
    );
  }
}
