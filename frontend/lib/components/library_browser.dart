import 'package:flutter/material.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';
import 'package:lef_editor_plugin/lef_editor_plugin.dart';

class LibraryBrowser extends StatefulWidget {
  const LibraryBrowser({super.key});

  @override
  State<LibraryBrowser> createState() => _LibraryBrowserState();
}

class _LibraryBrowserState extends State<LibraryBrowser> {
  late LeProvider _provider;
  final List<LeLibrary> _libraries = [];
  final List<LeDesignEntry> _designs = [];

  @override
  void initState() {
    super.initState();
    _provider = context.read<LeProvider>();
    _buildLibraryList();
  }

  Future<void> _buildLibraryList() async {
    final libraries = await _provider.getLibraries();
    if (!mounted) return;
    setState(() {
      _libraries
        ..clear()
        ..addAll(libraries);
    });
  }

  void _selectLibrary(int index) async {
    final designs = await _provider.getDesigns(index);
    if (!mounted) return;
    setState(() {
      _designs
        ..clear()
        ..addAll(designs);
    });
  }

  void _selectDesign(LeDesignRef id) async {
    _provider.openDesign(id);
    Navigator.of(context).pop();
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        return Padding(
          padding: const EdgeInsets.all(8.0),
          child: Row(
            spacing: 8,
            children: [
              Expanded(
                flex: 1,
                child: Column(
                  spacing: 8,
                  children: [
                    Text("Libraries (${_libraries.length})"),
                    Divider(),
                    Expanded(
                      child: ListView.builder(
                        itemCount: _libraries.length,
                        itemBuilder: (context, index) {
                          var library = _libraries[index];
                          return ListTile(
                            title: Text(library.name),
                            onTap: () => _selectLibrary(index),
                          );
                        },
                      ),
                    ),
                  ],
                ),
              ),
              Expanded(
                flex: 1,
                child: Column(
                  spacing: 8,
                  children: [
                    Text("Designs (${_designs.length})"),
                    Divider(),
                    Expanded(
                      child: ListView.builder(
                        itemCount: _designs.length,
                        itemBuilder: (context, index) {
                          var design = _designs[index];
                          return ListTile(
                            title: Text(design.name),
                            onTap: () => _selectDesign(design.id),
                          );
                        },
                      ),
                    ),
                  ],
                ),
              ),
            ],
          ),
        );
      },
    );
  }
}
