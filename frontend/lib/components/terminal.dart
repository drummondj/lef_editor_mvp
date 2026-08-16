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
const int kMaxResultDisplayLength = 10000;

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

  // Tab-completion (UPDATES.md item 20 point 3's GUI side) - asks the
  // backend's own complete_command (le_tcl_procs.tcl) for candidates
  // completing whichever whitespace-delimited token is currently being
  // typed (a command name, a -flag, or a .-prefixed property path - see
  // that proc's own doc comment), then either splices the sole candidate
  // in directly or, for more than one, lists them in the scrollback
  // without touching the input - the same "single match completes,
  // several just get listed" convention an interactive shell uses.
  // Silent (does nothing) on zero candidates, an empty input, or a
  // command still running - matches _submit()'s own guard.
  Future<void> _completeCommand() async {
    if (_running) return;
    final text = _inputController.text;
    if (text.trim().isEmpty) return;

    // complete_command takes the whole line as one Tcl string argument -
    // built here as a "..."-quoted literal (not {...}), since the
    // partial line being typed can itself contain an unbalanced brace
    // (e.g. "-rects {{0 0 1" mid-flag) that would break {...} grouping;
    // "..." only needs its own four special characters escaped, the
    // same set backend/codegen's tcl_dquote_escape() escapes server-side
    // for the same reason.
    final escaped = text
        .replaceAll('\\', '\\\\')
        .replaceAll('"', '\\"')
        .replaceAll('\$', '\\\$')
        .replaceAll('[', '\\[')
        .replaceAll(']', '\\]');

    String result;
    try {
      result = await _provider.runTclCommand('complete_command "$escaped"');
    } catch (_) {
      return;
    }
    final candidates = result.trim().isEmpty
        ? const <String>[]
        : result.trim().split(RegExp(r'\s+'));
    if (candidates.isEmpty) return;

    if (candidates.length == 1) {
      final completed = _replaceLastToken(text, candidates.first);
      setState(() {
        _inputController.text = completed;
        _inputController.selection = TextSelection.collapsed(
          offset: completed.length,
        );
      });
    } else {
      setState(() {
        _lines.addAll(_formatColumns(candidates));
      });
      _scrollToEnd();
    }
  }

  // Lays `items` out in justified columns, like a shell's own
  // multi-match completion listing - each column padded to the widest
  // item plus a 2-space gutter, wrapped once a row would exceed a
  // conventional 80-column terminal width. There's no real per-pixel
  // measurement of this console's own pane here (it's one resizable
  // pane of a docking layout, not the whole window), so 80 is an
  // assumption, not a measurement - the same fallback width a shell's
  // own completion menu uses when it can't detect the real terminal
  // size either. Relies on the app's own monospace theme font
  // (JetBrains Mono, see main.dart) for the padding to actually line up
  // visually.
  List<String> _formatColumns(List<String> items) {
    final maxLen = items.map((item) => item.length).reduce(
      (a, b) => a > b ? a : b,
    );
    final colWidth = maxLen + 2;
    const terminalWidth = 80;
    final columns = (terminalWidth / colWidth).floor().clamp(1, items.length);

    final rows = <String>[];
    for (var i = 0; i < items.length; i += columns) {
      final rowItems = items.skip(i).take(columns).toList();
      final row = StringBuffer();
      for (var j = 0; j < rowItems.length; j++) {
        final isLastInRow = j == rowItems.length - 1;
        row.write(isLastInRow ? rowItems[j] : rowItems[j].padRight(colWidth));
      }
      rows.add(row.toString());
    }
    return rows;
  }

  // Splices `candidate` in place of whichever trailing, whitespace-
  // delimited token of `text` is currently being typed - mirrors
  // complete_command's own "every candidate is a full replacement for
  // the last token" contract (le_tcl_procs.tcl) exactly, so this never
  // needs to know whether that token was a command name, a flag, or a
  // .-prefixed property path. Appends outright when `text` is empty or
  // already ends in whitespace (nothing partial to replace).
  String _replaceLastToken(String text, String candidate) {
    final match = RegExp(r'\S+$').firstMatch(text);
    if (match == null) {
      return '$text$candidate';
    }
    return text.substring(0, match.start) + candidate;
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
              KeyboardListener(
                focusNode: FocusNode(),
                // onKeyEvent: (value) {
                //   _inputFocusNode.requestFocus();
                // },
                child: SelectableText.rich(
                  scrollPhysics: NeverScrollableScrollPhysics(),
                  TextSpan(
                    style: DefaultTextStyle.of(context).style,
                    children: _lines
                        .map((message) => _messageToTextSpan(message))
                        .toList(),
                  ),
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
                      const SingleActivator(LogicalKeyboardKey.tab): () =>
                          _completeCommand(),
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
