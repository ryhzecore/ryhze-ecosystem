import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ryhze/core/app_failure.dart';
import 'package:ryhze/domain/contracts.dart';
import 'package:ryhze/features/entertainment/model/catalog.dart';
import 'package:ryhze/features/entertainment/widgets/search_panel.dart';
import 'package:ryhze/features/store/catalog_repository.dart';

class _DegradedCatalog implements CatalogRepository {
  _DegradedCatalog(this.quality);
  final ReadQuality quality;
  @override
  Future<RepositoryRead<EntertainmentTitle>> search(
    EntertainmentCategory category,
    String query,
    CancellationToken cancellation,
  ) async => RepositoryRead(
    [Catalog.larcenous],
    quality: quality,
    observedAt: DateTime.utc(2026),
    failure: const AppFailure(FailureKind.network),
  );
}

void main() {
  for (final quality in [ReadQuality.stale, ReadQuality.partial]) {
    testWidgets(
      'search labels $quality results without losing the visible title',
      (tester) async {
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SearchPanel(repository: _DegradedCatalog(quality)),
            ),
          ),
        );
        await tester.pumpAndSettle();
        expect(
          find.textContaining(
            quality == ReadQuality.stale
                ? 'previously fetched'
                : 'local catalogue',
          ),
          findsOneWidget,
        );
        expect(find.text('Larcenous Driftscape'), findsOneWidget);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }
}
