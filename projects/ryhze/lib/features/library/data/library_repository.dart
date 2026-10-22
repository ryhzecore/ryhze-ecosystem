import '../model/library_entry.dart';
import '../../../core/app_failure.dart';

/// A presentation boundary, not an ownership-provider implementation. An adapter
/// must establish its evidence before returning verified results. No catalogue,
/// favourite, directory scan or launch declaration is used as ownership proof.
abstract interface class LibraryRepository {
  Future<LibraryLoadResult> load();
}

abstract interface class CancellableLibraryRepository {
  Future<LibraryLoadResult> loadCancellable(CancellationToken cancellation);
}

enum LibraryResultKind { unavailable, verified, demo, failure }

/// Compatibility string arguments are never displayed. Failure messages come
/// from a closed vocabulary, even if an adapter passes a raw provider response.
class LibraryLoadResult {
  LibraryLoadResult.unavailable(String reason)
    : kind = LibraryResultKind.unavailable,
      message = reason == UnavailableLibraryRepository.reason
          ? UnavailableLibraryRepository.reason
          : const AppFailure(FailureKind.unsupported).message,
      entries = const [],
      partial = false,
      failure = const AppFailure(FailureKind.unsupported);

  LibraryLoadResult.verified(List<LibraryEntry> entries, {this.partial = false})
    : kind = LibraryResultKind.verified,
      failure = null,
      message = '',
      entries = List.unmodifiable(entries);

  LibraryLoadResult.demo(List<LibraryEntry> entries)
    : kind = LibraryResultKind.demo,
      failure = null,
      partial = false,
      message = 'Demo Library — fictional titles and simulated statuses.',
      entries = List.unmodifiable(entries);

  LibraryLoadResult.failure(String reason)
    : kind = LibraryResultKind.failure,
      failure = const AppFailure(FailureKind.network),
      message = const AppFailure(FailureKind.network).message,
      entries = const [],
      partial = false;

  LibraryLoadResult.failed(AppFailure safeFailure)
    : kind = LibraryResultKind.failure,
      failure = safeFailure,
      message = safeFailure.message,
      entries = const [],
      partial = false;

  final AppFailure? failure;
  final bool partial;
  final LibraryResultKind kind;
  final String message;
  final List<LibraryEntry> entries;
}

class UnavailableLibraryRepository implements LibraryRepository {
  const UnavailableLibraryRepository();

  static const reason =
      'Your Library is unavailable. No ownership provider is connected.';

  @override
  Future<LibraryLoadResult> load() async =>
      LibraryLoadResult.unavailable(reason);
}
