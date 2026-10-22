import 'app_route.dart';

enum ActivityKind { completed, handoff, recovery, failed, blocked }

class ActivityRecord {
  const ActivityRecord({
    required this.id,
    required this.kind,
    required this.occurredAt,
    required this.count,
    this.route,
    this.acknowledged = false,
  });
  final String id;
  final ActivityKind kind;
  final DateTime occurredAt;
  final int count;
  final AppRoute? route;
  final bool acknowledged;
  String get summary => switch (kind) {
    ActivityKind.completed => 'Operation completed.',
    ActivityKind.handoff =>
      'Provider handoff accepted. Running state is not verified.',
    ActivityKind.recovery => 'An interrupted operation needs recovery.',
    ActivityKind.failed => 'An operation failed. Review its current status.',
    ActivityKind.blocked => 'An operation is unavailable on this device.',
  };
  String preview({required bool private}) =>
      private ? 'Ryhze activity is available.' : summary;
  static ActivityRecord decode(String id, Map value) => ActivityRecord(
    id: id,
    kind: ActivityKind.values.byName(value['kind'] as String),
    occurredAt: DateTime.parse(value['occurredAt'] as String),
    count: value['count'] as int,
    acknowledged: value['acknowledged'] == true,
    route: value['route'] == null
        ? null
        : AppRoute.parse(Uri.parse(value['route'] as String)),
  );
}
