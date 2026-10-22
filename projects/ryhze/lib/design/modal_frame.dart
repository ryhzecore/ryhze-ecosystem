import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import 'tokens.dart';

/// Keeps enter/exit timing identical across search, menus and title details.
class RyhzeDialogRoute<T> extends RawDialogRoute<T> {
  RyhzeDialogRoute({
    required Widget child,
    required Duration enter,
    required this.exit,
  }) : super(
         barrierDismissible: true,
         barrierLabel: 'Close dialog',
         barrierColor: const Color(0x88000000),
         transitionDuration: enter,
         pageBuilder: (context, _, _) => BackdropFilter(
           filter: ui.ImageFilter.blur(sigmaX: 14, sigmaY: 14),
           child: SafeArea(child: child),
         ),
         transitionBuilder: (context, animation, _, child) => FadeTransition(
           opacity: animation.drive(CurveTween(curve: RyhzeTokens.ease)),
           child: ScaleTransition(
             scale: animation
                 .drive(CurveTween(curve: RyhzeTokens.ease))
                 .drive(Tween(begin: .985, end: 1.0)),
             child: child,
           ),
         ),
       );
  final Duration exit;
  @override
  Duration get reverseTransitionDuration => exit;
}
