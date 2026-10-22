import 'package:flutter/material.dart';

/// Shared native design contract with neutral surfaces and white actions.
abstract final class RyhzeTokens {
  static const background = Colors.black;
  static const surface = Color(0xFF111111);
  static const raised = Color(0xFF181818);
  static const text = Color(0xFFF7F7F7);
  static const muted = Color(0xFFC1C1C1);
  static const subtle = Color(0xFFA2A2A2);
  static const accent = Colors.white;
  static const accentSoft = Color(0xFFE6E6E6);
  static const focus = Colors.white;
  static const primary = Colors.white;
  static const onPrimary = Color(0xFF090909);
  static const primaryHover = Color(0xFFE6E6E6);
  static const secondaryHover = Color(0xFF262626);
  static const borderHover = Color(0xFF585858);
  static const border = Color(0xFF323232);
  static const radius = 32.0;
  static const itemRadius = 24.0;
  static const ease = Cubic(.22, .68, .18, 1);
  static const frameIn = Duration(milliseconds: 360);
  static const frameOut = Duration(milliseconds: 280);
  static const surfaceMotion = Duration(milliseconds: 550);
  static const spacing = 8.0;
  static const buttonShadow = [
    BoxShadow(color: Color(0x44000000), blurRadius: 8, offset: Offset(0, 3)),
  ];
  static const shadow = [
    BoxShadow(color: Color(0x44000000), blurRadius: 18, offset: Offset(0, 6)),
  ];

  static Duration motion(BuildContext context, Duration duration) =>
      MediaQuery.disableAnimationsOf(context) ? Duration.zero : duration;

  static ThemeData get theme => ThemeData(
    brightness: Brightness.dark,
    scaffoldBackgroundColor: background,
    fontFamily: 'Inter',
    colorScheme: const ColorScheme.dark(
      primary: accent,
      onPrimary: onPrimary,
      secondary: accent,
      onSecondary: onPrimary,
      surface: surface,
      onSurface: text,
      surfaceTint: Colors.transparent,
    ),
    useMaterial3: true,
    textSelectionTheme: const TextSelectionThemeData(cursorColor: accent),
    dividerColor: border,
    dividerTheme: const DividerThemeData(color: border, thickness: 1),
    dialogTheme: const DialogThemeData(
      backgroundColor: surface,
      surfaceTintColor: Colors.transparent,
    ),
    scrollbarTheme: const ScrollbarThemeData(interactive: true),
  );
}

TextStyle heading(double size) => TextStyle(
  fontFamily: 'SpaceGrotesk',
  fontSize: size,
  height: 1.08,
  fontWeight: FontWeight.w500,
  letterSpacing: -size * .055,
  color: RyhzeTokens.text,
);
