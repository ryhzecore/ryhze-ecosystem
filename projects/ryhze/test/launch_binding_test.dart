import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/platform/desktop_contracts.dart';

/// Planning fixture only; no native process or production capability is enabled.
class _LaunchCapableFixture implements DesktopAdapter {
  const _LaunchCapableFixture();

  @override
  bool supports(DesktopOperation operation) =>
      operation == DesktopOperation.launch;

  @override
  Future<ActionOutcome> execute(
    LaunchPlan plan,
    CancellationToken cancellation,
  ) async => throw StateError('Planning tests must not execute a launch.');
}

Matcher failsWith(FailureKind kind) =>
    throwsA(isA<AppFailure>().having((failure) => failure.kind, 'kind', kind));

void main() {
  final now = DateTime.utc(2026, 10, 10);
  final item = ProviderItemRef('provider', 'Title', namespace: 'catalogue');
  const account = 'account-A';
  const authority = 'approved-authority';
  const device = 'device-A';

  InstallRecord installation({
    ProviderItemRef? identity,
    String installer = 'provider',
    String installationDevice = device,
  }) => InstallRecord(
    device: installationDevice,
    installer: installer,
    item: identity ?? item,
    root: r'C:\Games\Title',
    relativeExecutable: r'bin\title.exe',
    inspectedAt: now,
  );

  Entitlement entitlement({
    String owner = account,
    String reportedAuthority = authority,
    AccessState state = AccessState.verified,
    DateTime? observedAt,
    DateTime? expiresAt,
    bool fixture = false,
  }) => Entitlement(
    account: owner,
    item: item,
    authority: reportedAuthority,
    observedAt: observedAt ?? now,
    expiresAt: expiresAt,
    state: state,
    fixture: fixture,
  );

  LaunchPlan prepare({
    InstallRecord? install,
    Entitlement? evidence,
    DesktopAdapter platform = const _LaunchCapableFixture(),
  }) => LaunchPlanner().prepare(
    platform: platform,
    install: install ?? installation(),
    entitlement: evidence ?? entitlement(),
    account: account,
    authority: authority,
    device: device,
    now: now,
  );

  test(
    'exact provider item namespace and qualified evidence permit planning',
    () {
      final plan = prepare(
        install: installation(
          identity: ProviderItemRef(
            'provider',
            'Title',
            namespace: 'catalogue',
          ),
        ),
      );
      expect(plan.item, item);
      expect(plan.target, r'C:\Games\Title\bin\title.exe');
      expect(plan.workingDirectory, r'C:\Games\Title');
      expect(plan.arguments, isEmpty);
    },
  );

  final otherIdentities = {
    'provider': ProviderItemRef('other', 'Title', namespace: 'catalogue'),
    'item': ProviderItemRef('provider', 'Other', namespace: 'catalogue'),
    'namespace': ProviderItemRef('provider', 'Title', namespace: 'other'),
    'provider case': ProviderItemRef(
      'Provider',
      'Title',
      namespace: 'catalogue',
    ),
    'item case': ProviderItemRef('provider', 'title', namespace: 'catalogue'),
    'namespace case': ProviderItemRef(
      'provider',
      'Title',
      namespace: 'Catalogue',
    ),
    'omitted namespace': ProviderItemRef('provider', 'Title'),
  };
  for (final entry in otherIdentities.entries) {
    final label = entry.key;
    test('installation with different $label cannot borrow entitlement', () {
      // Keep the reported installer valid to isolate exact item binding.
      expect(
        () => prepare(install: installation(identity: entry.value)),
        failsWith(FailureKind.invalidData),
      );
    });
  }

  test('correct item does not bypass owning installer semantics', () {
    expect(
      () => prepare(install: installation(installer: 'other')),
      failsWith(FailureKind.invalidData),
    );
  });

  test('correct item does not bypass device scope', () {
    expect(
      () => prepare(install: installation(installationDevice: 'device-B')),
      failsWith(FailureKind.invalidData),
    );
  });

  final deniedEvidence = {
    'wrong account': entitlement(owner: 'account-B'),
    'wrong authority': entitlement(reportedAuthority: 'other-authority'),
    'empty authority': entitlement(reportedAuthority: ''),
    'revoked': entitlement(state: AccessState.revoked),
    'expired state': entitlement(state: AccessState.expired),
    'unknown': entitlement(state: AccessState.unknown),
    'unavailable': entitlement(state: AccessState.unavailable),
    'expired at boundary': entitlement(expiresAt: now),
    'future observation': entitlement(
      observedAt: now.add(const Duration(seconds: 1)),
    ),
    'demonstration evidence': entitlement(fixture: true),
  };
  for (final entry in deniedEvidence.entries) {
    final label = entry.key;
    test('exact installed identity cannot override $label', () {
      expect(
        () => prepare(evidence: entry.value),
        failsWith(FailureKind.denied),
      );
    });
  }

  test(
    'malformed provider or item is rejected before installation planning',
    () {
      for (final parts in [
        ['', 'Title'],
        ['provider', ''],
        ['provider\u0000', 'Title'],
        ['provider', 'Title\u0000'],
      ]) {
        expect(
          () => installation(identity: ProviderItemRef(parts[0], parts[1])),
          failsWith(FailureKind.invalidData),
        );
      }
    },
  );

  test(
    'production unavailable adapter stays unavailable with matching facts',
    () {
      const platform = UnavailableDesktopAdapter();
      expect(platform.supports(DesktopOperation.launch), isFalse);
      expect(
        () => prepare(platform: platform),
        failsWith(FailureKind.unsupported),
      );
    },
  );
}
