#ifndef RUNNER_CONTENT_TRANSFER_H_
#define RUNNER_CONTENT_TRANSFER_H_

#include "secure_files.h"
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ryhze {
struct ContentTransferRequest {
  uint32_t slot = 0, attempt = 0;
  std::string operation_id;
  uint64_t expected_size = 0, observed_bytes = 0;
  secure_files::FileDigest expected_sha256{};
  std::string root_identity, directory_identity, source_identity;
  std::string observed_sha256;
};
struct ContentTransferResult {
  // Empty error is success. Closed values: invalidData, conflict, recovery,
  // cancelled, storage, unsupported. Never expose paths or OS diagnostics.
  std::string error;
  std::string state, root_identity, directory_identity, source_identity;
  std::string source_relative, observed_sha256;
  uint64_t observed_bytes = 0;
};

// One synchronous owner, used only on the host's serial background worker.
// Cancel alone is thread-safe. Drain must follow worker settlement and closes
// the writer without deleting any bytes. No root can come from channel data.
class ContentTransfer {
 public:
  static constexpr uint32_t kSlotCount = 8;
  static constexpr size_t kChunkBytes = 256 * 1024;
  static constexpr uint64_t kMaximumBytes = 1024ull * 1024 * 1024;
  explicit ContentTransfer(std::wstring private_root);
  ~ContentTransfer();
  ContentTransfer(const ContentTransfer&) = delete;
  ContentTransfer& operator=(const ContentTransfer&) = delete;
  ContentTransferResult Begin(const ContentTransferRequest& request,
                             const std::optional<ContentTransferRequest>& resume = {});
  ContentTransferResult Append(const ContentTransferRequest& request,
                              const std::vector<uint8_t>& bytes);
  ContentTransferResult Finish(const ContentTransferRequest& request);
  ContentTransferResult Inspect(const ContentTransferRequest& request);
  ContentTransferResult Discard(const ContentTransferRequest& request);
  void Cancel() noexcept;
  void Drain();
  // Called by the serial host owner before a new operation after settlement.
  void ResetCancellation();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::string ContentTransferIdentity(const secure_files::FileIdentity& value);
bool ValidContentTransferRequest(const ContentTransferRequest& request);
}  // namespace ryhze
#endif
