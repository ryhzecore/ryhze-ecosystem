import 'package:flutter/material.dart';

import 'tokens.dart';

/// Flat page background; keeps the shared wrapper without an ambient overlay.
class RyhzeAmbience extends StatelessWidget {
  const RyhzeAmbience({super.key, required this.child});
  final Widget child;
  @override
  Widget build(BuildContext context) =>
      ColoredBox(color: RyhzeTokens.background, child: child);
}

/// Branded concept art, deliberately never presented as a gameplay screenshot.
class ProjectArtwork extends StatelessWidget {
  const ProjectArtwork({
    super.key,
    this.cinema = false,
    this.logoWidthFactor = .58,
    this.logoHeightFactor = .85,
    this.bottomShade = true,
  });
  final bool cinema;
  final double logoWidthFactor, logoHeightFactor;
  final bool bottomShade;
  @override
  Widget build(BuildContext context) => RepaintBoundary(
    child: ExcludeSemantics(
      child: DecoratedBox(
        decoration: const BoxDecoration(color: RyhzeTokens.raised),
        child: Stack(
          fit: StackFit.expand,
          children: [
            Center(
              child: FractionallySizedBox(
                widthFactor: logoWidthFactor,
                heightFactor: logoHeightFactor,
                child: cinema
                    ? const Icon(
                        Icons.movie_outlined,
                        size: 120,
                        color: Color(0x29FFFFFF),
                      )
                    : Opacity(
                        opacity: .1,
                        child: Image.asset(
                          'assets/brand/symbol-480.webp',
                          cacheWidth: 480,
                          fit: BoxFit.contain,
                        ),
                      ),
              ),
            ),
            if (bottomShade)
              const DecoratedBox(
                decoration: BoxDecoration(
                  gradient: LinearGradient(
                    begin: Alignment.topCenter,
                    end: Alignment.bottomCenter,
                    colors: [Colors.transparent, Color(0xD9000000)],
                  ),
                ),
              ),
          ],
        ),
      ),
    ),
  );
}

/// Shared artwork border and clipping, without a glow outside the frame.
class ArtworkFrame extends StatelessWidget {
  const ArtworkFrame({
    super.key,
    required this.child,
    this.focused = false,
    this.framed = true,
  });
  final Widget child;
  final bool focused;
  final bool framed;
  @override
  Widget build(BuildContext context) => !framed
      ? child
      : DecoratedBox(
          position: DecorationPosition.foreground,
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(RyhzeTokens.radius),
            border: Border.all(
              color: focused ? RyhzeTokens.focus : RyhzeTokens.border,
            ),
          ),
          child: ClipRRect(
            borderRadius: BorderRadius.circular(RyhzeTokens.radius - 1),
            child: child,
          ),
        );
}
