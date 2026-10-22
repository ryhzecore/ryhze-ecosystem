#pragma once
#include "desktop_project_browser.hpp"
#include "content_mesh_checkpoint.hpp"
#include "mesh_reimport.hpp"
#include <functional>
#include <optional>
#include <thread>

namespace race::desktop {
inline constexpr std::size_t maximum_checkpoint_retained_bytes=512*1024;
struct MeshReimportRequest {
  // Every retained root is limited to 4096 bytes of size AND capacity.
  // staging_root names an existing parent; the factory creates a fresh child.
  std::string source_root,product_root,checkpoint_root,staging_root;
  std::uint64_t epoch{};
};
struct MeshReimportWorker {
  std::shared_ptr<platform::RootedFiles> stage;
  std::string executable,directory;
  std::unique_ptr<platform::process::Service> process;
};
enum class MeshReimportPhase : std::uint8_t {
  Idle,Revalidating,ReadingSource,Starting,Importing,Publishing,Capturing,Stopping,Complete
};
struct MeshReimportProgress {
  MeshReimportPhase phase{MeshReimportPhase::Idle};
  std::size_t source_bytes{};
  bool cancellation_requested{};
};
struct MeshReimportCheckpoint {
  std::shared_ptr<const ProjectBrowserView> project;
  MeshReimportRequest roots;
  mesh_checkpoint::Published published;
  MeshReimportCheckpoint(std::shared_ptr<const ProjectBrowserView> view,
      MeshReimportRequest names,mesh_checkpoint::Published result) noexcept
      :project(std::move(view)),roots(std::move(names)),published(std::move(result)){}
};
struct MeshReimportCompletion {
  std::uint64_t request{},epoch{};
  assets::AssetId selected{};
  assets::Digest source{};
  std::string staging_directory; // Bounded retained evidence path, when prepared.
  Result<void> result;
  bool changed{};
  std::size_t source_bytes{},product_bytes{},retained_bytes{};
  double admission_ms{},revalidate_ms{},source_ms{},import_ms{},cook_publish_ms{},capture_ms{},publication_ms{};
};
// Trusted executable/staging/native-path authority belongs to executable
// composition. The factory must create the Service on the calling worker,
// supply a never-reused service identity, and pin a fresh stage whose directory
// is its exact actual root outside source/products. No automatic Scene edits.
// Fixed profiles: source 8 MiB, each product 16 MiB, Store payload 64 MiB,
// project snapshot 256 KiB; these are separate budgets, not total process RSS.
class MeshReimportTask {
public:
  using RootFactory=ProjectBrowserTask::RootFactory;
  using WorkerFactory=std::function<Result<MeshReimportWorker>(std::uint64_t,const MeshReimportRequest&)>;
private:
  struct Pending;
  const std::thread::id owner_{std::this_thread::get_id()};
  RootFactory roots_;
  WorkerFactory workers_;
  std::uint64_t sequence_{};
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit MeshReimportTask(RootFactory={},WorkerFactory={});
  ~MeshReimportTask();
  MeshReimportTask(const MeshReimportTask&)=delete;
  MeshReimportTask& operator=(const MeshReimportTask&)=delete;
  bool busy() const noexcept{return worker_.joinable();} // Owner observation.
  Result<std::uint64_t> start(std::shared_ptr<const ProjectBrowserView>,assets::AssetId,MeshReimportRequest);
  // Sticky signal only. Reads/child streams stay owned until physical join.
  Result<void> cancel();
  Result<MeshReimportProgress> progress() const noexcept;
  Result<std::optional<MeshReimportCompletion>> poll(std::shared_ptr<const ProjectBrowserView> current,
      assets::AssetId selected,std::uint64_t epoch,std::shared_ptr<const MeshReimportCheckpoint>& checkpoint);
};
} // namespace race::desktop
