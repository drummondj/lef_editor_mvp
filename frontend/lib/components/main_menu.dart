import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:lef_editor/components/library_browser.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

class MainMenu extends StatefulWidget {
  const MainMenu({super.key});

  @override
  State<MainMenu> createState() => _MainMenuState();
}

class _MainMenuState extends State<MainMenu> {
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
          child: SizedBox(
            width: MediaQuery.of(context).size.width * 0.66,
            height: MediaQuery.of(context).size.height * 0.9,
            child: LibraryBrowser(),
          ),
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
    return Padding(
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
    );
  }
}
