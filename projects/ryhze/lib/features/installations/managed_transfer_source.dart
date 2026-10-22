import '../../core/app_failure.dart';
import '../../providers/managed_content_transport.dart';
import 'managed_content_admission.dart';
import 'managed_content_manifest.dart';

/// An authenticated adapter binds a concrete transport to the same access read.
/// This type is an assertion by trusted composition, not proof by construction.
/// URLs, cookies and bearer material never enter the durable transfer record.
class ManagedTransferSource {
  ManagedTransferSource({
    required this.request,
    required this.provenance,
    required this.sourceKey,
    required this.transport,
    required this.fixture,
  }) {
    if (RegExp(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$')
            .firstMatch(sourceKey)
            ?.end !=
        sourceKey.length) {
      throw const AppFailure(FailureKind.invalidData);
    }
  }
  final ManagedContentAccessRequest request;
  final ManagedManifestProvenance provenance;
  final String sourceKey;
  final ManagedContentTransport transport;
  final bool fixture;

  void check(
    ManagedContentAccessRequest expected,
    ManagedContentAdmissionGuard admission, {
    String? expectedSourceKey,
  }) {
    admission.check();
    if (fixture ||
        request.scope.key != expected.scope.key ||
        request.sessionGeneration != expected.sessionGeneration ||
        request.deviceId != expected.deviceId ||
        !request.intent.sameRequest(expected.intent) ||
        !provenance.sameArtifact(admission.provenance) ||
        provenance.observedAt != admission.provenance.observedAt ||
        provenance.expiresAt != admission.provenance.expiresAt ||
        (expectedSourceKey != null && sourceKey != expectedSourceKey)) {
      throw const AppFailure(FailureKind.denied);
    }
  }
}

abstract interface class ManagedContentTransferAuthority
    implements ManagedContentAuthority {
  /// Must settle actual work after cancellation. The returned destination is
  /// independently authenticated by the selected service; UI cannot supply it.
  Future<ManagedTransferSource> resolveTransfer(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  );
}

class UnavailableManagedContentTransferAuthority
    extends UnavailableManagedContentAuthority
    implements ManagedContentTransferAuthority {
  const UnavailableManagedContentTransferAuthority();
  @override
  Future<ManagedTransferSource> resolveTransfer(
    ManagedContentAccessRequest request,
    CancellationToken cancellation,
  ) async {
    cancellation.check();
    throw const AppFailure(FailureKind.unsupported);
  }
}
