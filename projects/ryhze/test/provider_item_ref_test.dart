import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';

void main() {
  test(
    'provider namespace rejects oversized and control-bearing identities',
    () {
      for (final namespace in [
        'x' * 257,
        for (var control = 0; control < 32; control++)
          'scope${String.fromCharCode(control)}item',
      ]) {
        expect(
          () => ProviderItemRef('provider', 'item', namespace: namespace),
          throwsA(isA<AppFailure>()),
        );
      }
    },
  );

  test('provider namespace remains optional, bounded and exactly scoped', () {
    final implicit = ProviderItemRef('provider', 'item');
    final explicit = ProviderItemRef('provider', 'item', namespace: '');
    expect(implicit, explicit);
    expect(implicit.hashCode, explicit.hashCode);
    expect(
      ProviderItemRef('provider', 'item', namespace: 'x' * 256).namespace,
      'x' * 256,
    );
    final upper = ProviderItemRef('provider', 'item', namespace: 'Scope');
    final lower = ProviderItemRef('provider', 'item', namespace: 'scope');
    final spaced = ProviderItemRef('provider', 'item', namespace: ' scope ');
    expect({implicit, upper, lower, spaced}, hasLength(4));
    expect(spaced.namespace, ' scope ');
  });
}
