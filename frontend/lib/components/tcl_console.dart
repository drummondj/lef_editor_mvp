import 'package:flutter/material.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

/// Collapsible Tcl command console (TCL_EXPLORATION.md's show_gui design) -
/// visually mirrors [MessageConsole]'s collapsible-panel pattern, but a
/// separate widget: this one is interactive (a command line, not just a
/// read-only log) and talks to [LeProvider.runTclCommand], not
/// [LeProvider.messages].
class TclConsole extends StatefulWidget {
  const TclConsole({super.key, this.expandedHeight = 200});

  final double expandedHeight;

  @override
  State<TclConsole> createState() => _TclConsoleState();
}

class _TclConsoleState extends State<TclConsole> {
  final ScrollController _scrollController = ScrollController();
  final TextEditingController _inputController = TextEditingController();
  final FocusNode _inputFocusNode = FocusNode();
  final List<String> _lines = [];
  bool _collapsed = true;
  bool _running = false;

  @override
  void dispose() {
    _scrollController.dispose();
    _inputController.dispose();
    _inputFocusNode.dispose();
    super.dispose();
  }

  void _scrollToEnd() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_scrollController.hasClients) return;
      _scrollController.jumpTo(_scrollController.position.maxScrollExtent);
    });
  }

  Future<void> _submit(LeProvider provider) async {
    final command = _inputController.text.trim();
    if (command.isEmpty || _running) return;
    setState(() {
      _lines.add('% $command');
      _inputController.clear();
      _running = true;
    });
    _scrollToEnd();

    String result;
    try {
      result = await provider.runTclCommand(command);
    } catch (error) {
      result = 'error: $error';
    }

    setState(() {
      if (result.isNotEmpty) _lines.add(result);
      _running = false;
    });
    _scrollToEnd();
    _inputFocusNode.requestFocus();
  }

  @override
  Widget build(BuildContext context) {
    final provider = context.read<LeProvider>();
    const monospace = TextStyle(fontFamily: 'JetBrains Mono', fontSize: 12);

    return AnimatedContainer(
      duration: const Duration(milliseconds: 150),
      height: _collapsed ? 32 : widget.expandedHeight,
      color: Colors.black,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          InkWell(
            onTap: () => setState(() => _collapsed = !_collapsed),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 8.0, vertical: 6.0),
              child: Row(
                children: [
                  Icon(
                    _collapsed ? Icons.expand_less : Icons.expand_more,
                    color: Colors.white70,
                    size: 18,
                  ),
                  const SizedBox(width: 4),
                  const Text(
                    'Tcl Console',
                    style: TextStyle(color: Colors.white70, fontSize: 12),
                  ),
                ],
              ),
            ),
          ),
          if (!_collapsed) ...[
            Expanded(
              child: Padding(
                padding: const EdgeInsets.fromLTRB(8.0, 0, 8.0, 4.0),
                child: SingleChildScrollView(
                  controller: _scrollController,
                  child: SelectableText(
                    _lines.join('\n'),
                    style: monospace.copyWith(color: Colors.white70),
                  ),
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(8.0, 0, 8.0, 8.0),
              child: TextField(
                controller: _inputController,
                focusNode: _inputFocusNode,
                enabled: !_running,
                style: monospace.copyWith(color: Colors.white),
                cursorColor: Colors.white70,
                decoration: const InputDecoration(
                  isDense: true,
                  prefixText: '% ',
                  prefixStyle: monospace,
                  border: OutlineInputBorder(),
                ),
                onSubmitted: (_) => _submit(provider),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
