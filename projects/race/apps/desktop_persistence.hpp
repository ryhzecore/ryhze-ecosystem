#pragma once
#include "desktop_scene.hpp"
#include <functional>
#include <optional>
#include <thread>

namespace race::desktop {
enum class PersistenceKind { Save, Cook, Load, Checkpoint, Recover, PackageLoad };
constexpr bool reads_scene(PersistenceKind kind) noexcept {
  return kind==PersistenceKind::Load||kind==PersistenceKind::Recover||kind==PersistenceKind::PackageLoad;
}
struct PersistenceCompletion {
  std::uint64_t request{};
  PersistenceKind kind{};
  PersistenceStamp stamp;
  std::string path;
  Result<void> result;
  std::optional<scene::Document> loaded;
  bool matches_current{};
  double snapshot_ms{},worker_ms{};
};
// One owner thread, one retained request, one terminal completion. Files is
// exclusively worker-owned. No detached task, timeout cleanup, or queued writes.
// A save already admitted may finish its atomic replacement; only loads support
// cancellation of publication. Recover shares Load's admission contract;
// Checkpoint writes the published authoring snapshot without changing its state.
// Destruction joins any physical I/O still active.
class PersistenceTask {
  struct Pending;
  std::unique_ptr<platform::Files> files_;
  const std::thread::id owner_{std::this_thread::get_id()};
  std::uint64_t sequence_{};
  PersistenceKind kind_{};
  bool cancelled_{};
  // The owned thread publishes readiness only after local/input cleanup. The
  // owner acquires readiness and joins before reading terminal metadata.
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit PersistenceTask(std::unique_ptr<platform::Files>);
  ~PersistenceTask();
  PersistenceTask(const PersistenceTask&)=delete;
  PersistenceTask& operator=(const PersistenceTask&)=delete;
  bool busy() const { return worker_.joinable(); } // Owner observation only.
  // Optional application dependency preparation runs on the same joined worker.
  // It must capture owned immutable inputs, never mutate the live Scene.
  using Prepare=std::function<Result<void>(const scene::Document&)>;
  // A package decoder is supplied by executable composition; it owns any
  // prepared dependencies until the joined completion is published or discarded.
  using Decode=std::function<Result<scene::Document>(platform::Files&,std::string_view,const scene::Schemas&)>;
  Result<std::uint64_t> start(const Document&,PersistenceKind,std::string path,Prepare={},Decode={});
  Result<void> cancel_load();
  // Nonblocking while physical work is active. Installs only on this owner.
  Result<std::optional<PersistenceCompletion>> poll(Document&);
};
}
