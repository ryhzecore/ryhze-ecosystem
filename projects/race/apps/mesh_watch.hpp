#pragma once
#include "mesh_reimport.hpp"
#include "race/platform/directory_watch.hpp"

namespace race::mesh_import {
struct PreparedWorker {
  std::shared_ptr<platform::RootedFiles> stage;
  std::string executable, directory;
  std::unique_ptr<platform::process::Service> process;
};
// Executable composition creates a fresh stage and never-reused process service
// per charged attempt. Output and stages must be outside the watched source root.
class WorkerFactory {
public:
  virtual ~WorkerFactory()=default;
  virtual Result<PreparedWorker> prepare(std::uint32_t attempt)=0;
};
struct WatchLimits {
  std::uint32_t imports{8}, read_retries{3}; // 1..64, 1..8
  bool coalesce_identical{}; // opt-in until the paired workload is measured
};
enum class WatchPhase : std::uint8_t {Running,Stopping,Stopped};
enum class WatchOutcome : std::uint8_t {None,Invalidated,Admitted,Published,Rejected,SourceUnavailable,Unchanged,Stopped};
struct WatchStatus {
  WatchPhase phase{WatchPhase::Running};
  WatchOutcome outcome{WatchOutcome::None};
  platform::directory_watch::Observation observed{};
  std::uint64_t events{},source_reads{},desired_generation{},superseded{},unchanged{};
  std::uint32_t attempts{},read_failures{};
  bool worker_retained{};
  assets::Digest source{},current_revision{};
  std::size_t retained_bytes{},pending_source_bytes{};
  std::optional<Error> last_error,terminal_error;
};
// One owner thread, one source leaf, one child, no queue or background callback.
// Poll may synchronously read/cook/publish; this is an authoring-tool controller,
// not a Qt callback. Invalidations precede completion admission. Acknowledgement
// certifies only the observation, never a transactional filesystem snapshot.
class Watch {
  std::thread::id thread_{std::this_thread::get_id()};
  WatchLimits limits_;
  WatchStatus status_;
  std::string leaf_;
  bool scan_pending_{},watch_stopped_{};
  std::optional<Desired> active_;
  std::shared_ptr<platform::RootedFiles> source_;
  std::unique_ptr<WorkerFactory> factory_;
  std::unique_ptr<platform::directory_watch::Service> watch_;
  // Reverse destruction quiesces the child before the watcher and capabilities.
  std::unique_ptr<Reimport> owner_;
  Watch()=default;
  void event(WatchOutcome,std::optional<Error> = {});
  void stop(std::optional<Error> = {});
  void refresh();
public:
  // Takes an idle reimport owner, optionally with an existing current revision.
  static Result<std::unique_ptr<Watch>> create(std::shared_ptr<platform::RootedFiles>,std::string_view,
      std::unique_ptr<platform::directory_watch::Service>,std::unique_ptr<WorkerFactory>,
      std::unique_ptr<Reimport>,const WatchLimits& = {});
  ~Watch()=default;
  Result<WatchStatus> poll();
  Result<void> request_stop();
  Result<assets::mesh::ResourceHandle> current() const {return owner_->current();}
  Result<assets::mesh::Lease> acquire(assets::mesh::ResourceHandle h) const {return owner_->acquire(h);}
};
} // namespace race::mesh_import
