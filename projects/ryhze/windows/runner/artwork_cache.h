#ifndef RUNNER_ARTWORK_CACHE_H_
#define RUNNER_ARTWORK_CACHE_H_
#include "secure_files.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ryhze {
enum class ArtworkError { none, invalid, unavailable, conflict, integrity, quota, cancelled, io };
struct ArtworkRecord { std::vector<uint8_t> bytes; int64_t stored_at_ms = 0; };
struct ArtworkHealth { size_t entries = 0; uint64_t bytes = 0; size_t preserved = 0; };
struct ArtworkClear { size_t removed = 0; size_t preserved = 0; };

// Inert public artwork only. Checksums detect corruption, not authenticity or
// entitlement. Single-thread ownership; a process-wide file lease serializes
// cooperating writers. Unknown or damaged entries are never overwritten/deleted.
class ArtworkCache {
 public:
  static constexpr size_t kMaximumEntry = 8 * 1024 * 1024;
  static constexpr uint64_t kMaximumBytes = 64ull * 1024 * 1024;
  static constexpr size_t kMaximumEntries = 64;
  static constexpr size_t kMaximumScan = 128;
  static constexpr size_t kHeaderBytes = 120;
  static constexpr int64_t kMaximumTimestamp = 4102444800000;
  using Cancelled = std::function<bool()>;
  explicit ArtworkCache(std::optional<std::wstring> private_parent = std::nullopt);
  static bool ValidKey(const std::string& key);
  bool Ready(ArtworkError* error);
  bool Get(const std::string& key, std::optional<ArtworkRecord>* record,
           ArtworkError* error, const Cancelled& cancelled = {});
  bool Put(const std::string& key, const std::vector<uint8_t>& bytes, int64_t stored_at_ms,
           ArtworkError* error, const Cancelled& cancelled = {});
  bool Health(ArtworkHealth* result, ArtworkError* error, const Cancelled& cancelled = {});
  bool Clear(ArtworkClear* result, ArtworkError* error, const Cancelled& cancelled = {});
 private:
  std::optional<std::wstring> parent_;
  std::optional<secure_files::Root> root_;
  std::optional<secure_files::FileIdentity> identity_;
  std::optional<secure_files::DirectoryLease> Open(bool removal, ArtworkError* error);
};
}  // namespace ryhze
#endif
