import '../model/library_entry.dart';
import 'library_repository.dart';

/// Fictional, explicitly marked fixtures. This repository never verifies real
/// ownership or performs provider, filesystem, installation or launch work.
class DemoLibraryRepository implements LibraryRepository {
  const DemoLibraryRepository();

  static LibraryEntry _entry(
    String id,
    String title,
    String description,
    List<String> tags,
    LibraryInstallationState installation,
    String installationReason,
  ) => LibraryEntry(
    identity: LibraryIdentity(providerId: 'ryhze-demo', titleId: id),
    title: title,
    description: description,
    tags: tags,
    ownership: const LibraryOwnership(
      state: LibraryOwnershipState.demonstration,
      reason: 'Fictional demo record. This is not verified ownership.',
    ),
    installation: LibraryInstallation(
      state: installation,
      reason: installationReason,
    ),
    launchSupport: const LibraryActionSupport(
      state: LibraryActionState.unsupported,
      reason: 'Demo only. No game can be launched.',
    ),
    installSupport: const LibraryActionSupport(
      state: LibraryActionState.unsupported,
      reason: 'Demo only. No download or installation is available.',
    ),
  );

  @override
  Future<LibraryLoadResult> load() async => LibraryLoadResult.demo([
    _entry(
      'aurora-orchard',
      'Aurora Orchard',
      'A fictional garden among the stars, created only to demonstrate Library browsing.',
      ['Fictional demo', 'Exploration'],
      LibraryInstallationState.installed,
      'Simulated installed status. No installed game was detected.',
    ),
    _entry(
      'copper-compass',
      'Copper Compass',
      'A fictional journey across mechanical islands. This title has no playable build.',
      ['Fictional demo', 'Adventure'],
      LibraryInstallationState.notInstalled,
      'Simulated not-installed status. Installation is not implemented.',
    ),
    _entry(
      'moonlit-parcel',
      'Moonlit Parcel',
      'An imaginary delivery route used to demonstrate unknown installation status.',
      ['Fictional demo', 'Puzzle'],
      LibraryInstallationState.unknown,
      'Simulated unknown status. Device installation has not been checked.',
    ),
    _entry(
      'tideglass-trail',
      'Tideglass Trail',
      'A fictional coastal trail used to demonstrate an unsupported device check.',
      ['Fictional demo', 'Exploration'],
      LibraryInstallationState.unsupported,
      'Simulated unsupported status. No installation adapter is implemented.',
    ),
    _entry(
      'velvet-voyage',
      'Velvet Voyage',
      'An imaginary voyage used to demonstrate a failed installation-status check.',
      ['Fictional demo', 'Adventure'],
      LibraryInstallationState.failed,
      'Simulated check failure. No actual provider or device check was attempted.',
    ),
  ]);
}
