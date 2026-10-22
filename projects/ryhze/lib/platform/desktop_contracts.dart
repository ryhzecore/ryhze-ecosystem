import '../core/app_failure.dart';
import '../domain/contracts.dart';

enum DesktopOperation {
  secureCredentials,
  files,
  launch,
  protocol,
  notifications,
  lifecycle,
  updates,
}

enum ActionOutcome {
  unavailable,
  denied,
  cancelled,
  handoffAccepted,
  processObserved,
  unknown,
}

abstract interface class DesktopAdapter {
  bool supports(DesktopOperation operation);
  Future<ActionOutcome> execute(
    LaunchPlan plan,
    CancellationToken cancellation,
  );
}

/// Fail closed until a native adapter passes its operation-specific acceptance.
/// Being on Windows does not enable an operation by itself.
class UnavailableDesktopAdapter implements DesktopAdapter {
  const UnavailableDesktopAdapter();
  @override
  bool supports(DesktopOperation operation) => false;
  @override
  Future<ActionOutcome> execute(
    LaunchPlan plan,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    return ActionOutcome.unavailable;
  }
}

class InstallRecord {
  const InstallRecord({
    required this.device,
    required this.installer,
    required this.item,
    required this.root,
    required this.relativeExecutable,
    required this.inspectedAt,
  });
  final String device, installer, root, relativeExecutable;
  final ProviderItemRef item;
  final DateTime inspectedAt;
}

/// No shell string exists. Construction is private to validated planning.
class LaunchPlan {
  LaunchPlan._(this.target, this.arguments, this.workingDirectory, this.item);
  final String target;
  final List<String> arguments;
  final String workingDirectory;
  final ProviderItemRef item;
}

class LaunchPlanner {
  LaunchPlan prepare({
    required DesktopAdapter platform,
    required InstallRecord install,
    required Entitlement entitlement,
    required String account,
    required String authority,
    required String device,
    required DateTime now,
  }) {
    if (!platform.supports(DesktopOperation.launch)) {
      throw const AppFailure(FailureKind.unsupported);
    }
    if (!entitlement.qualifies(
      accountScope: account,
      expectedAuthority: authority,
      now: now,
    )) {
      throw const AppFailure(FailureKind.denied);
    }
    if (install.item != entitlement.item ||
        install.device != device ||
        install.installer != entitlement.item.provider ||
        install.inspectedAt.isAfter(now) ||
        now.difference(install.inspectedAt) > const Duration(minutes: 5) ||
        !WindowsPathPolicy.validRoot(install.root) ||
        !WindowsPathPolicy.validRelative(install.relativeExecutable) ||
        !install.relativeExecutable.toLowerCase().endsWith('.exe') ||
        const {
          'cmd.exe',
          'powershell.exe',
          'pwsh.exe',
          'wscript.exe',
          'cscript.exe',
          'mshta.exe',
          'rundll32.exe',
        }.contains(
          install.relativeExecutable
              .replaceAll('\\', '/')
              .split('/')
              .last
              .toLowerCase(),
        )) {
      throw const AppFailure(FailureKind.invalidData);
    }
    // A native adapter must revalidate canonical file identity, junctions and
    // volatile access immediately before executing this preliminary plan.
    return LaunchPlan._(
      '${install.root}\\${install.relativeExecutable}',
      const [],
      install.root,
      entitlement.item,
    );
  }
}

abstract final class WindowsPathPolicy {
  static final _reserved = RegExp(
    r'^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)',
    caseSensitive: false,
  );
  static bool validRelative(String path) {
    if (path.isEmpty ||
        path.length > 240 ||
        path.startsWith('/') ||
        path.startsWith('\\')) {
      return false;
    }
    return path
        .replaceAll('\\', '/')
        .split('/')
        .every(
          (part) =>
              part.isNotEmpty &&
              part != '.' &&
              part != '..' &&
              !part.endsWith('.') &&
              !part.endsWith(' ') &&
              !_reserved.hasMatch(part) &&
              !RegExp(r'[<>:"|?*\x00-\x1f]').hasMatch(part),
        );
  }

  static bool validRoot(String root) =>
      RegExp(r'^[A-Za-z]:\\').hasMatch(root) &&
      validRelative(root.substring(3));
}

class ProviderDestinationPolicy {
  ProviderDestinationPolicy(Map<String, Set<String>> hosts)
    : _hosts = Map.unmodifiable(
        hosts.map(
          (key, value) => MapEntry(key, Set<String>.unmodifiable(value)),
        ),
      );
  final Map<String, Set<String>> _hosts;
  bool permits(String provider, Uri uri) =>
      uri.scheme == 'https' &&
      !uri.hasPort &&
      uri.userInfo.isEmpty &&
      uri.fragment.isEmpty &&
      (_hosts[provider]?.contains(uri.host) ?? false) &&
      uri.queryParameters.isEmpty &&
      !uri.pathSegments.any(
        (part) => part == '..' || part.contains('\\') || part.contains('/'),
      );
}

/// Optional products negotiate separately. Neither receives private context.
class OptionalIntegration {
  const OptionalIntegration(this.name, {this.contractVersion = 1});
  final String name;
  final int contractVersion;
  bool get available => false;
  String get reason => 'No authorized $name adapter is configured.';
}
