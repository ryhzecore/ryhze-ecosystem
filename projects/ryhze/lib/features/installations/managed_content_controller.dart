import 'package:flutter/foundation.dart';

import '../../core/app_failure.dart';
import '../session/session_controller.dart';
import '../jobs/job_coordinator.dart';
import 'managed_content_service.dart';
import 'managed_content_discard_service.dart';
import 'managed_installation.dart';
import 'managed_installation_repository.dart';

class ManagedContentController extends ChangeNotifier {
  ManagedContentController({
    required this.repository,
    required this.service,
    required this.session,
    this.coordinator,
    this.discardService,
  }) {
    _refresh();
    repository.addListener(_repositoryChanged);
    session.addListener(_scopeChanged);
  }
  final ManagedInstallationRepository repository;
  final ManagedContentService service;
  final SessionController session;
  final JobCoordinator? coordinator;
  final ManagedContentDiscardService? discardService;
  ManagedDiscardConfirmation? removalConfirmation;
  String? removalActivity;
  static const pageSize = 20;
  int _page = 0;
  bool _disposed = false;
  CancellationToken? _request;
  String? activeOperationId;
  bool resuming = false;
  int? _activeGeneration;
  bool get activeInCurrentScope =>
      busy && _activeGeneration == session.generation;
  AppFailure? failure;
  bool cancellationRequested = false;
  bool get busy => _request != null;
  List<ManagedInstallationRecord> _records = const [];
  List<ManagedInstallationRecord> get records => _records;
  void _refresh() {
    final result = repository.readCurrent().toList()
      ..sort((a, b) => a.operationId.compareTo(b.operationId));
    _records = List.unmodifiable(result);
    _page = page;
  }

  void _repositoryChanged() {
    if (_disposed) return;
    _refresh();
    removalConfirmation = null;
    notifyListeners();
  }

  int get pageCount =>
      ((records.length + pageSize - 1) ~/ pageSize).clamp(1, 7);
  int get page => _page.clamp(0, pageCount - 1);
  List<ManagedInstallationRecord> get visibleRecords =>
      List.unmodifiable(records.skip(page * pageSize).take(pageSize));
  void previousPage() {
    if (page > 0) {
      _page = page - 1;
      _changed();
    }
  }

  void nextPage() {
    if (page + 1 < pageCount) {
      _page = page + 1;
      _changed();
    }
  }

  bool canInspect(ManagedInstallationRecord record) =>
      !busy &&
      (record.state == ManagedInstallationState.staged ||
          record.state == ManagedInstallationState.recoveryRequired);

  bool canResume(ManagedInstallationRecord record) =>
      !busy &&
      coordinator != null &&
      record.state == ManagedInstallationState.recoveryRequired &&
      service.canResume(record.operationId);

  Future<void> inspect(String operationId) => _run(operationId, resume: false);
  Future<void> resumeRecord(String operationId) =>
      _run(operationId, resume: true);

  Future<void> _run(String operationId, {required bool resume}) async {
    if (_disposed || busy || removalConfirmation != null) return;
    final record = records
        .where((record) => record.operationId == operationId)
        .firstOrNull;
    if (record == null || (resume ? !canResume(record) : !canInspect(record))) {
      return;
    }
    final generation = session.generation;
    final token = _request = CancellationToken();
    activeOperationId = operationId;
    _activeGeneration = generation;
    resuming = resume;
    cancellationRequested = false;
    failure = null;
    notifyListeners();
    try {
      if (resume) {
        await service.resumeRecord(
          operationId,
          token,
          coordinator: coordinator!,
        );
      } else {
        await service.inspectRecord(operationId, token);
      }
    } catch (error) {
      if (!_disposed &&
          !token.isCancelled &&
          generation == session.generation) {
        failure = error is AppFailure
            ? error
            : const AppFailure(FailureKind.recovery);
      }
    } finally {
      if (!_disposed && identical(_request, token)) {
        _request = null;
        activeOperationId = null;
        _activeGeneration = null;
        resuming = false;
        cancellationRequested = false;
        notifyListeners();
      }
    }
  }

  bool canReviewRemoval(ManagedInstallationRecord record) =>
      !busy &&
      removalConfirmation == null &&
      discardService != null &&
      coordinator != null &&
      (record.state == ManagedInstallationState.staged ||
          record.state == ManagedInstallationState.recoveryRequired ||
          record.state == ManagedInstallationState.discardRecoveryRequired);

  bool isRemovalCurrent(ManagedDiscardConfirmation confirmation) =>
      !_disposed &&
      identical(removalConfirmation, confirmation) &&
      discardService!.isCurrent(confirmation);

  void dismissRemoval() {
    removalConfirmation = null;
    if (!_disposed) notifyListeners();
  }

  Future<ManagedDiscardConfirmation?> reviewRemoval(String id) async {
    final record = records
        .where((record) => record.operationId == id)
        .firstOrNull;
    if (_disposed || record == null || !canReviewRemoval(record)) return null;
    await _removalWork(id, 'Reviewing retained staging files…', (token) async {
      final confirmation =
          record.state == ManagedInstallationState.discardRecoveryRequired
          ? await discardService!.inspectRecord(id, token)
          : await discardService!.prepareRecord(id, token);
      if (!token.isCancelled &&
          confirmation != null &&
          discardService!.isCurrent(confirmation)) {
        removalConfirmation = confirmation;
      }
    });
    return removalConfirmation;
  }

  Future<void> confirmRemoval(ManagedDiscardConfirmation confirmation) async {
    if (busy || !isRemovalCurrent(confirmation)) return;
    removalConfirmation = null;
    await _removalWork(
      confirmation.operationId,
      'Removing retained staging copy…',
      (token) async {
        if (confirmation.recovery) {
          await discardService!.resumeDiscardRecord(
            confirmation,
            token,
            coordinator: coordinator!,
          );
        } else {
          await discardService!.discardRecord(
            confirmation,
            token,
            coordinator: coordinator!,
          );
        }
      },
    );
  }

  Future<void> _removalWork(
    String id,
    String activity,
    Future<void> Function(CancellationToken) work,
  ) async {
    final generation = session.generation;
    final token = _request = CancellationToken();
    activeOperationId = id;
    _activeGeneration = generation;
    removalActivity = activity;
    cancellationRequested = false;
    failure = null;
    notifyListeners();
    try {
      await work(token);
    } catch (error) {
      if (!_disposed &&
          !token.isCancelled &&
          generation == session.generation) {
        failure = error is AppFailure
            ? error
            : const AppFailure(FailureKind.recovery);
      }
    } finally {
      if (!_disposed && identical(_request, token)) {
        _request = null;
        activeOperationId = null;
        _activeGeneration = null;
        removalActivity = null;
        cancellationRequested = false;
        notifyListeners();
      }
    }
  }

  void cancel() {
    if (_disposed || !busy) return;
    cancellationRequested = true;
    _request!.cancel();
    notifyListeners();
  }

  void _scopeChanged() {
    _request?.cancel();
    cancellationRequested = busy;
    activeOperationId = null;
    failure = null;
    removalConfirmation = null;
    _page = 0;
    _refresh();
    _changed();
  }

  void _changed() {
    if (_disposed) return;
    _page = page;
    notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    repository.removeListener(_repositoryChanged);
    session.removeListener(_scopeChanged);
    _request?.cancel();
    super.dispose();
  }
}
