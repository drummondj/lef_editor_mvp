import 'package:flutter/material.dart';

class WidgetCard extends StatelessWidget {
  final Widget child;

  const WidgetCard({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(8.0),
      child: Card(
        shape: RoundedRectangleBorder(
          side: BorderSide(
            color: const Color.fromARGB(255, 39, 39, 39),
            width: 2.0,
          ),
          borderRadius: BorderRadiusGeometry.circular(13),
        ),
        color: Colors.black.withAlpha(150),
        child: Padding(padding: const EdgeInsets.all(8.0), child: child),
      ),
    );
  }
}
