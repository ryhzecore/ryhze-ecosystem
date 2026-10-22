#pragma once
#include "race/platform/rooted_files.hpp"
#include <optional>

namespace race::platform::directory_watch {
struct Observation {
  std::uint64_t watcher{},generation{};
  friend bool operator==(const Observation&,const Observation&)=default;
};
struct Limits {std::uint64_t generations{~std::uint64_t{0}};};
enum class Phase : std::uint8_t {Watching,Stopping,Stopped};
struct Snapshot {
  Observation observation;
  Phase phase{Phase::Watching};
  // Dirty always requires re-reading the caller's declared source set. This is
  // an invalidation hint, not a journal, filename list or stable-file barrier.
  bool dirty{true};
  // Batches include immediate overflow reports on rearm, not just completed
  // native I/O requests. One poll may observe a completion plus rearm overflow.
  std::uint64_t completed_batches{},overflow_batches{},coalesced_batches{};
  std::optional<Error> failure;
};
class Service {
public:
  virtual ~Service()=default;
  // Owner-thread, one completion/rearm maximum, no deliberate wait. Initial
  // dirty generation requires a rescan after arming; counters count batches.
  virtual Result<Snapshot> poll() noexcept=0;
  // Poll once before checking the exact observation. A later native change can
  // arrive after acknowledgement; this does not certify filesystem stability.
  virtual Result<void> acknowledge(Observation) noexcept=0;
  virtual Result<void> request_stop() noexcept=0;
  // Only after request_stop; at most 60,000 ms. Timeout retains every resource.
  virtual Result<Snapshot> wait_stopped(std::uint32_t milliseconds) noexcept=0;
};
// One pinned existing local NTFS root, nonrecursive, fixed 16 KiB native buffer,
// one overlapped request, no callbacks or background threads. Every completed
// change batch invalidates; lost details have an explicit overflow counter.
// Caller supplies a never-reused nonzero watcher identity. Foreign capabilities
// are Unsupported. Destruction cancels and waits for I/O before freeing the
// retained root, native buffer, event or request state. Not a sandbox or journal.
Result<std::unique_ptr<Service>> make_windows_service(
    std::shared_ptr<RootedFiles>,std::uint64_t,const Limits& = {});
} // namespace race::platform::directory_watch
