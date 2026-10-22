import 'package:flutter/material.dart';

import '../../../design/controls.dart';
import '../../../design/tokens.dart';
import '../../../platform/capabilities.dart';

class AboutPanel extends StatelessWidget {
  const AboutPanel({super.key});
  @override
  Widget build(BuildContext context) => Dialog(
    shape: RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(RyhzeTokens.radius),
      side: const BorderSide(color: RyhzeTokens.border),
    ),
    child: ConstrainedBox(
      constraints: const BoxConstraints(maxWidth: 520),
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Eyebrow('Ryhze'),
            const SizedBox(height: 8),
            const Text(
              'Version 0.6.0',
              style: TextStyle(color: RyhzeTokens.muted),
            ),
            const SizedBox(height: 16),
            Text('Entertainment,\ntaking shape.', style: heading(36)),
            const SizedBox(height: 24),
            Text(
              'Running on ${PlatformCapabilities.current.name}.',
              style: const TextStyle(color: RyhzeTokens.muted),
            ),
            const SizedBox(height: 16),
            const Text(
              'This is a development foundation. No games, films, account sign-in, provider connections or downloads are available yet.',
              style: TextStyle(color: RyhzeTokens.subtle, height: 1.8),
            ),
            const SizedBox(height: 24),
            RyhzeControl(
              label: 'Close about',
              autofocus: true,
              onPressed: () => Navigator.pop(context),
              child: const Text('Back'),
            ),
          ],
        ),
      ),
    ),
  );
}
