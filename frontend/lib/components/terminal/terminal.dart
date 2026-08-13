import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

const Map<String, Color> kMessageColors = {
  "ERROR": Colors.redAccent,
  "WARNING": Colors.amberAccent,
  "INFO": Colors.greenAccent,
};

const String prompt = "le_shell";

// A single misbehaving command (e.g. a bare get_shapes on a design with
// thousands of shapes) shouldn't be able to dump megabytes of text into
// the terminal's scrollback - truncate what's *displayed*, not the value
// itself, so a script relying on the full result is unaffected.
const int kMaxResultDisplayLength = 1000;

String _truncateForDisplay(String text) {
  if (text.length <= kMaxResultDisplayLength) return text;
  return '${text.substring(0, kMaxResultDisplayLength)}..truncated';
}

class Terminal extends StatefulWidget {
  const Terminal({super.key});

  @override
  State<Terminal> createState() => _TerminalState();
}

class _TerminalState extends State<Terminal> {
  late final LeProvider _provider;
  final ScrollController _scrollController = ScrollController();
  final TextEditingController _inputController = TextEditingController();
  final FocusNode _inputFocusNode = FocusNode();
  final List<String> _lines = [];
  bool _running = false;
  int _nextCommandNumber = 1;
  final List<String> _commandHistory = [];
  int _currentHistoryIndex = -1;
  late StreamSubscription<String> _messageSubscription;

  @override
  void initState() {
    super.initState();
    _provider = context.read<LeProvider>();
    _messageSubscription = _provider.addMessageListener((message) {
      setState(() {
        _lines.add(message);
        _scrollToEnd();
      });
    });
  }

  @override
  void dispose() {
    _messageSubscription.cancel();
    _scrollController.dispose();
    _inputController.dispose();
    _inputFocusNode.dispose();
    super.dispose();
  }

  TextSpan _messageToTextSpan(String message) {
    for (var name in kMessageColors.keys) {
      var match = RegExp(
        "^$name(.*)",
        caseSensitive: false,
      ).firstMatch(message);
      if (match != null) {
        return TextSpan(
          children: [
            TextSpan(
              text: name,
              style: TextStyle(color: kMessageColors[name]),
            ),
            TextSpan(text: "${match.group(1)}\n"),
          ],
        );
      }
    }
    if (!message.endsWith("\n")) {
      message += "\n";
    }
    return TextSpan(text: message);
  }

  Future<void> _submit() async {
    final command = _inputController.text.trim();
    if (command.isEmpty || _running) return;
    if (command == "history") {
      var padding = (_nextCommandNumber - 1).toString().length;
      setState(() {
        _lines.add("");
        _lines.addAll(
          _commandHistory.indexed.map(
            (e) => "${(e.$1 + 1).toString().padLeft(padding)}  ${e.$2}",
          ),
        );
        _lines.add("");
      });
    } else {
      setState(() {
        _lines.add('$_nextCommandNumber> $command');
        _commandHistory.add(command);
        _nextCommandNumber++;
        _running = true;
      });

      String result;
      try {
        result = await _provider.runTclCommand(command);
      } catch (error) {
        result = 'error: $error';
      }

      setState(() {
        if (result.isNotEmpty) _lines.add(_truncateForDisplay(result));
        _running = false;
      });
    }
    _scrollToEnd();
    _currentHistoryIndex = -1;
    _inputController.clear();
    _inputFocusNode.requestFocus();
  }

  void _scrollToEnd() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!_scrollController.hasClients) return;
      _scrollController.jumpTo(_scrollController.position.maxScrollExtent);
    });
  }

  void _history(int by) {
    if (_commandHistory.isEmpty) {
      return;
    }
    int index = -1;
    if (_currentHistoryIndex == -1) {
      index = _nextCommandNumber - 1 + by;
    } else {
      index = _currentHistoryIndex + by;
    }

    if (index < 0 || index > _commandHistory.length - 1) {
      return;
    }

    _inputController.clear();
    _inputController.text = _commandHistory[index];
    _currentHistoryIndex = index;
  }

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8.0),
      child: SingleChildScrollView(
        controller: _scrollController,
        child: Column(
          crossAxisAlignment: .stretch,
          children: [
            if (_lines.isNotEmpty)
              SelectableText.rich(
                scrollPhysics: NeverScrollableScrollPhysics(),
                TextSpan(
                  style: DefaultTextStyle.of(context).style,
                  children: _lines
                      .map((message) => _messageToTextSpan(message))
                      .toList(),
                ),
              ),
            Row(
              mainAxisSize: .min,
              crossAxisAlignment: .start,
              children: [
                Text("$prompt % "),
                Expanded(
                  child: CallbackShortcuts(
                    bindings: <ShortcutActivator, VoidCallback>{
                      const SingleActivator(LogicalKeyboardKey.arrowUp): () =>
                          _history(-1),
                      const SingleActivator(LogicalKeyboardKey.arrowDown): () =>
                          _history(1),
                      const SingleActivator(LogicalKeyboardKey.enter): () =>
                          _submit(),
                    },
                    child: TextField(
                      controller: _inputController,
                      focusNode: _inputFocusNode,
                      enabled: !_running,
                      cursorColor: Colors.white70,
                      cursorWidth: 10,
                      decoration: const InputDecoration(
                        isDense: true,
                        border: UnderlineInputBorder(
                          borderSide: BorderSide.none,
                        ),
                      ),
                      style: DefaultTextStyle.of(context).style,
                      maxLines: null,
                      onSubmitted: (_) => _submit(),
                      textAlignVertical: .top,
                    ),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
