import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

class Home extends StatefulWidget {
  const Home({super.key});

  @override
  State<Home> createState() => _HomeState();
}

class _HomeState extends State<Home> {
  Size? _lastViewportSize;

  @override
  void initState() {
    super.initState();
    context.read<LeProvider>().init();
  }

  void _handleViewportResize(BoxConstraints constraints) {
    final size = Size(constraints.maxWidth, constraints.maxHeight);
    if (size == _lastViewportSize) return;
    _lastViewportSize = size;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) context.read<LeProvider>().resize(size);
    });
  }

  Future<void> openFilePicker() async {
    const typeGroup = XTypeGroup(label: 'LEF files', extensions: ['lef']);
    final files = await openFiles(acceptedTypeGroups: [typeGroup]);
    if (files.isEmpty || !mounted) return;
    final provider = context.read<LeProvider>();
    for (final file in files) {
      await provider.readLef(file.path);
    }
  }

  void openDesign(int index) async {
    await context.read<LeProvider>().openDesign(index);
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
                        crossAxisAlignment: .center,
                        children: [
                          FilledButton.icon(
                            icon: Icon(Icons.folder_open),
                            onPressed: openFilePicker,
                            label: Text("Open LEF ..."),
                          ),
                        ],
                      ),
                    ),
                  ),
                  Expanded(
                    flex: 1,
                    child: Column(
                      mainAxisAlignment: .start,
                      mainAxisSize: .min,
                      children: [
                        if (provider.openDesigns.isEmpty) ...[
                          Text("No designs loaded"),
                        ],
                        Expanded(
                          child: ListView.builder(
                            itemCount: provider.openDesigns.length,
                            itemBuilder: (context, index) {
                              var designName = provider.openDesigns[index];
                              return ListTile(
                                title: Text(designName),
                                onTap: () => openDesign(index),
                              );
                            },
                          ),
                        ),
                      ],
                    ),
                  ),
                  Expanded(
                    flex: 5,
                    child: LayoutBuilder(
                      builder: (context, constraints) {
                        _handleViewportResize(constraints);
                        return SizedBox(
                          width: constraints.maxWidth,
                          height: constraints.maxHeight,
                          child: DecoratedBox(
                            decoration: BoxDecoration(
                              border: Border.all(color: Colors.grey),
                            ),
                            child: provider.texture != null
                                ? Texture(
                                    textureId: provider.texture!.textureId,
                                  )
                                : null,
                          ),
                        );
                      },
                    ),
                  ),
                ],
              ),
            );
          },
        ),
      ),
    );
  }
}
