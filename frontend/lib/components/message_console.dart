import 'package:flutter/material.dart';
import 'package:lef_editor/components/status_bar.dart';
import 'package:lef_editor/providers/le_provider.dart';
import 'package:provider/provider.dart';

class MessageConsole extends StatefulWidget {
  const MessageConsole({super.key, this.expandedHeight = 200});

  final double expandedHeight;

  static const Map<String, Color> colors = {
    "ERROR": Colors.redAccent,
    "WARNING": Colors.amberAccent,
    "INFO": Colors.greenAccent,
  };

  @override
  State<MessageConsole> createState() => _MessageConsoleState();
}

class _MessageConsoleState extends State<MessageConsole> {
  final ScrollController scrollController = ScrollController();
  bool _collapsed = false;

  TextSpan _messageToTextSpan(String message) {
    for (var name in MessageConsole.colors.keys) {
      var match = RegExp(
        "^$name(.*)",
        caseSensitive: false,
      ).firstMatch(message);
      if (match != null) {
        return TextSpan(
          children: [
            TextSpan(
              text: name,
              style: TextStyle(color: MessageConsole.colors[name]),
            ),
            TextSpan(text: "${match.group(1)}\n"),
          ],
        );
      }
    }
    return TextSpan(text: "$message\n");
  }

  void _scrollToEnd() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!scrollController.hasClients) return;
      scrollController.jumpTo(scrollController.position.maxScrollExtent);
    });
  }

  @override
  Widget build(BuildContext context) {
    return Consumer<LeProvider>(
      builder: (context, provider, child) {
        _scrollToEnd();
        return Column(
          children: [
            StatusBar(),
            SizedBox(height: 8),
            AnimatedContainer(
              duration: const Duration(milliseconds: 150),
              height: _collapsed ? 32 : widget.expandedHeight,
              color: Colors.black,
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  InkWell(
                    onTap: () => setState(() => _collapsed = !_collapsed),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                        horizontal: 8.0,
                        vertical: 6.0,
                      ),
                      child: Row(
                        children: [
                          Icon(
                            _collapsed ? Icons.expand_less : Icons.expand_more,
                            color: Colors.white70,
                            size: 18,
                          ),
                          const SizedBox(width: 4),
                          const Text(
                            "Console",
                            style: TextStyle(
                              color: Colors.white70,
                              fontSize: 12,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                  if (!_collapsed)
                    Expanded(
                      child: Padding(
                        padding: const EdgeInsets.fromLTRB(8.0, 0, 8.0, 8.0),
                        child: SingleChildScrollView(
                          controller: scrollController,
                          child: SelectableText.rich(
                            TextSpan(
                              style: DefaultTextStyle.of(context).style,
                              children: provider.messages
                                  .map((message) => _messageToTextSpan(message))
                                  .toList(),
                            ),
                          ),
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}
