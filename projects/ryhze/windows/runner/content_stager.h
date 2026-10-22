#ifndef RUNNER_CONTENT_STAGER_H_
#define RUNNER_CONTENT_STAGER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ryhze {

using ContentDigest = std::array<uint8_t, 32>;
enum class ContentStageState {
  staged, checkpoint, cancelled, invalid, conflict, recovery, quota, unavailable
};

struct ContentStageRequest {
  uint32_t slot = 0;
  ContentDigest operation_id{};
  ContentDigest manifest_sha256{};
  // Relative to the injected private root's fixed "incoming" directory.
  std::wstring source_relative;
  uint64_t expected_size = 0;
  ContentDigest expected_sha256{};
  // Stage requires identities obtained from a preceding Inspect; Inspect may
  // leave these empty to obtain its initial read-only observation.
  std::string expected_root_identity;
  std::string expected_source_identity;
  // Stage must reread at least this retained prefix before creating any slot or
  // chunk. Zero permits a fresh stage. This per-attempt floor is not serialized
  // into the immutable checkpoint; Inspect reports the actual prefix even when
  // it is shorter. Must be chunk-aligned or exactly expected_size.
  uint64_t minimum_verified_bytes = 0;
};

struct ContentStageResult {
  ContentStageState state = ContentStageState::unavailable;
  uint64_t verified_bytes = 0;
  uint32_t verified_chunks = 0;
  // Observed while pinned; lowercase hex volume (16), ':', file ID (32).
  // Empty until observed. These are identities, not future mutation authority.
  std::string root_identity;
  std::string source_identity;
};

struct ContentDiscardPlanResult {
  ContentStageState state = ContentStageState::unavailable;
  std::vector<uint8_t> plan;
  uint64_t verified_bytes = 0;
  uint32_t verified_chunks = 0;
};
enum class ContentDiscardState { ready, completed, cancelled, invalid, conflict, recovery, unavailable };
struct ContentDiscardResult {
  ContentDiscardState state = ContentDiscardState::unavailable;
  // Cumulative planned payload bytes/files observed absent, including previous
  // invocations. Checkpoint counts as one file but not as payload bytes.
  uint64_t removed_bytes = 0;
  uint32_t removed_files = 0;
};

// Inert copying of a caller-identified local payload, not authenticated content
// distribution. A hash/manifest identity does not establish rights or a publisher.
// Requests cannot select a native root; composition supplies the private root
// and provisions its existing incoming and content-staging directories.
// Eight retained slots bound generated payload to 8 GiB, plus bounded metadata.
// Stage never deletes, overwrites or auto-evicts retained files. Explicit discard
// removes only the exact destination files covered by its durable verified plan.
class ContentStager {
 public:
  static constexpr uint32_t kSlotCount = 8;
  static constexpr uint32_t kChunkBytes = 256 * 1024;
  static constexpr uint64_t kMaximumBytes = 1024ull * 1024 * 1024;
  static constexpr uint64_t kDiskReserveBytes = 64ull * 1024 * 1024;
  static constexpr size_t kMaximumDiscardPlanBytes = 512 * 1024;
  explicit ContentStager(std::wstring private_root);

  // Check cancellation between bounded reads and flushed chunk checkpoints.
  // Caller runs this synchronous primitive on its managed background worker.
  // Callback exceptions are treated as cancellation. Progress is reread-verified
  // in this invocation and may decrease after corruption on a later inspection.
  ContentStageResult Stage(const ContentStageRequest& request,
                           const std::function<bool()>& cancelled = {});
  // Revalidates complete source identity/hash and every retained chunk. Does not
  // create a lease/slot, resume copying, or provision either private directory.
  // An unused slot returns checkpoint(0) plus the pinned identities. A checkpoint
  // alone never authorizes skipping validation.
  ContentStageResult Inspect(const ContentStageRequest& request,
                             const std::function<bool()>& cancelled = {});

  // Read-only fresh validation. Requires a present exact slot, valid complete
  // source and contiguous verified prefix; any unknown entry refuses planning.
  ContentDiscardPlanResult PlanDiscard(const ContentStageRequest& request,
                                       const std::function<bool()>& cancelled = {});
  // Caller must durably commit this opaque plan as explicit discard intent
  // before invoking. The checksum detects corruption, not hostile fabrication
  // or authorization. Never accept an untrusted externally supplied plan.
  ContentDiscardResult Discard(const std::vector<uint8_t>& plan,
                               const std::function<bool()>& cancelled = {});
  ContentDiscardResult InspectDiscard(const std::vector<uint8_t>& plan,
                                      const std::function<bool()>& cancelled = {});

 private:
  ContentStageResult Run(const ContentStageRequest& request, bool copy,
                         const std::function<bool()>& cancelled);
  ContentDiscardResult RunDiscard(const std::vector<uint8_t>& plan, bool remove,
                                  const std::function<bool()>& cancelled);
  std::wstring private_root_;
};

}  // namespace ryhze

#endif  // RUNNER_CONTENT_STAGER_H_
