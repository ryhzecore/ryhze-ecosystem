import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/session/session_controller.dart';
import 'package:ryhze/providers/paged_entitlement_source.dart';
import 'package:ryhze/providers/provider_request_policy.dart';

class FixturePages implements EntitlementPageAdapter {
  FixturePages(this.pages);
  final List<Object> pages;
  int calls = 0;
  @override
  String get authority => 'fixture-authority';
  @override
  String get provider => 'fixture-provider';
  @override
  bool get fixture => true;
  @override
  Future<EntitlementPage> fetch(
    AccountScope scope,
    String? cursor,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    final value = pages[calls++];
    if (value is EntitlementPage) return value;
    throw value;
  }
}

void main() {
  const scope = AccountScope.local();
  final now = DateTime.utc(2026, 10, 7);
  Entitlement record(String id, {bool fixture = true}) => Entitlement(
    account: scope.key,
    item: ProviderItemRef('fixture-provider', id),
    authority: 'fixture-authority',
    observedAt: now,
    state: AccessState.verified,
    fixture: fixture,
  );
  test(
    'retry waits honor Retry-After, stop on authentication, and bound delay',
    () async {
      final waits = <Duration>[];
      final policy = ProviderRequestPolicy(
        random: () => 0.5,
        wait: (duration, token) async {
          waits.add(duration);
          token.check();
        },
      );
      var calls = 0;
      final result = await policy.run(CancellationToken(), () async {
        if (calls++ == 0) {
          throw const ProviderRequestFailure(
            AppFailure(FailureKind.network),
            retryAfter: Duration(seconds: 2),
          );
        }
        return 'ok';
      });
      expect(result, 'ok');
      expect(waits, [const Duration(seconds: 2)]);
      await expectLater(
        policy.run(
          CancellationToken(),
          () async => throw const ProviderRequestFailure(
            AppFailure(FailureKind.authentication),
          ),
        ),
        throwsA(isA<AppFailure>()),
      );
      expect(waits, hasLength(1));
      await expectLater(
        policy.run(
          CancellationToken(),
          () async => throw const ProviderRequestFailure(
            AppFailure(FailureKind.network),
            retryAfter: Duration(minutes: 5),
          ),
        ),
        throwsA(isA<AppFailure>()),
      );
      expect(waits, hasLength(1));
    },
  );
  test(
    'cancellation interrupts a pending backoff without another request',
    () async {
      final token = CancellationToken();
      final pending = ProviderRequestPolicy.cancellableWait(
        const Duration(minutes: 1),
        token,
      );
      final assertion = expectLater(
        pending,
        throwsA(
          isA<AppFailure>().having(
            (e) => e.kind,
            'kind',
            FailureKind.cancelled,
          ),
        ),
      );
      token.cancel();
      await assertion;
    },
  );
  test('paged fixtures remain labelled; incomplete outage cannot become empty ownership', () async {
    final adapter = FixturePages([
      EntitlementPage([record('A')], nextCursor: 'next'),
      const AppFailure(FailureKind.network),
    ]);
    final result = await PagedEntitlementSource(
      adapter,
      clock: () => now,
    ).read(scope, CancellationToken());
    expect(result.quality, ReadQuality.partial);
    expect(result.values.single.fixture, isTrue);
    final empty = await PagedEntitlementSource(
      FixturePages([const AppFailure(FailureKind.network)]),
      clock: () => now,
    ).read(scope, CancellationToken());
    expect(empty.quality, ReadQuality.unavailable);
  });
  test(
    'duplicate, mixed, and repeating cursor pages are rejected as invalid data',
    () async {
      for (final pages in [
        [
          EntitlementPage([record('A')], nextCursor: 'next'),
          EntitlementPage([record('A')]),
        ],
        [
          EntitlementPage([record('A'), record('B', fixture: false)]),
        ],
        [
          EntitlementPage([record('A')], nextCursor: 'loop'),
          EntitlementPage([record('B')], nextCursor: 'loop'),
        ],
      ]) {
        final result = await PagedEntitlementSource(
          FixturePages(pages),
          clock: () => now,
        ).read(scope, CancellationToken());
        expect(result.quality, ReadQuality.unavailable);
        expect(result.values, isEmpty);
        expect(result.failure!.kind, FailureKind.invalidData);
      }
    },
  );
  test('page budget reports partial, not complete', () async {
    final result = await PagedEntitlementSource(
      FixturePages([
        EntitlementPage([record('A')], nextCursor: 'more'),
      ]),
      maxPages: 1,
      clock: () => now,
    ).read(scope, CancellationToken());
    expect(result.quality, ReadQuality.partial);
    expect(result.values, hasLength(1));
  });
}
