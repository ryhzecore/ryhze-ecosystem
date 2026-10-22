#ifndef RUNNER_UPDATE_STAGER_H_
#define RUNNER_UPDATE_STAGER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ryhze {
using UpdateDigest = std::array<uint8_t, 32>;
enum class UpdateStageState { unavailable, invalid, untrusted, staged, recovery, conflict };
struct UpdateStageResult {
  UpdateStageState state = UpdateStageState::unavailable;
  uint64_t bytes = 0;
};

// A local, non-executing gate. No production publisher is trusted by default.
// A staged result is an observation, never authorization to execute a path later.
// The caller must separately establish feed/version/rollback/activation policy.
class UpdateStager {
 public:
  static constexpr size_t kMaximumBytes = 16 * 1024 * 1024;
  explicit UpdateStager(std::wstring private_root,
      std::optional<UpdateDigest> publisher_der_sha256 = std::nullopt);
  UpdateStageResult Stage(const std::vector<uint8_t>& bytes,
                          const UpdateDigest& expected_sha256);
  UpdateStageResult Inspect(const UpdateDigest& expected_sha256,
                            uint64_t expected_size);
 private:
  std::wstring private_root_;
  std::optional<UpdateDigest> publisher_der_sha256_;
};
}  // namespace ryhze
#endif  // RUNNER_UPDATE_STAGER_H_
