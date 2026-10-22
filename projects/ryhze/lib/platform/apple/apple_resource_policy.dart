/// Validation is a prerequisite to an adapter action, never permission to
/// install games, launch executables, or infer ownership from local files.
final class ApplePolicyException implements Exception {
  const ApplePolicyException();
  @override
  String toString() => 'This resource is not permitted.';
}

final class AppleResourcePolicy {
  AppleResourcePolicy({Iterable<String> allowedHosts = const []})
    : allowedHosts = Set.unmodifiable(allowedHosts) {
    if (this.allowedHosts.any((host) => !_publicAsciiHost(host))) {
      throw const ApplePolicyException();
    }
  }
  final Set<String> allowedHosts;

  static bool _publicAsciiHost(String host) {
    if (host != host.toLowerCase() ||
        host.length > 253 ||
        !host.contains('.') ||
        host.endsWith('.') ||
        RegExp(r'^[0-9.]+$').hasMatch(host) ||
        host.endsWith('.localhost') ||
        host.endsWith('.local') ||
        host.endsWith('.internal') ||
        host.endsWith('.test') ||
        host.endsWith('.invalid')) {
      return false;
    }
    return host
        .split('.')
        .every(
          (part) =>
              RegExp(r'^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$').hasMatch(part),
        );
  }

  bool allowsNetworkUri(Uri uri) {
    if (uri.scheme != 'https' ||
        !uri.hasAuthority ||
        uri.userInfo.isNotEmpty ||
        uri.hasQuery ||
        uri.hasFragment ||
        uri.port != 443 ||
        !_publicAsciiHost(uri.host) ||
        !allowedHosts.contains(uri.host)) {
      return false;
    }
    final text = uri.toString();
    if (RegExp(r'[\x00-\x20\x7f\\]').hasMatch(text) ||
        RegExp(
          r'%(?:0[0-9a-f]|1[0-9a-f]|7f|5c)',
          caseSensitive: false,
        ).hasMatch(text)) {
      return false;
    }
    for (final segment in uri.pathSegments) {
      if (segment == '.' ||
          segment == '..' ||
          segment.contains('/') ||
          segment.contains('\\') ||
          segment.contains('%')) {
        return false;
      }
    }
    return true;
  }

  Uri requireNetworkUri(Uri uri) {
    if (!allowsNetworkUri(uri)) throw const ApplePolicyException();
    return uri;
  }

  /// External navigation has its own adapter gate; this never opens a URL.
  bool allowsExternalUri(Uri uri) => allowsNetworkUri(uri);

  /// Native callers must still anchor to their sandbox and reject symlinks at
  /// open time. A lexical check alone cannot prevent filesystem races.
  String requireRelativeFilePath(String path) {
    if (path.isEmpty ||
        path.length > 1024 ||
        path.startsWith('/') ||
        path.contains('\\') ||
        path.contains(':') ||
        path.contains('%') ||
        RegExp(r'[\x00-\x1f\x7f]').hasMatch(path) ||
        path
            .split('/')
            .any((part) => part.isEmpty || part == '.' || part == '..')) {
      throw const ApplePolicyException();
    }
    return path;
  }

  /// Only reviewed bundled artwork is available in the current foundation.
  bool allowsBundledMedia(String assetPath) => const {
    'assets/brand/symbol-480.webp',
    'assets/brand/wordmark-348.webp',
  }.contains(assetPath);
}
