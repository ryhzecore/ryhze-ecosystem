#pragma once
#include "desktop_project_browser.hpp"
#include "surface_checkpoint.hpp"
#include <functional>
#include <optional>
#include <thread>

namespace race::desktop {
inline constexpr std::size_t maximum_surface_checkpoint_retained_bytes=512*1024;
struct SurfaceReimportRequest {
  std::string source_root,product_root,checkpoint_root; // size and capacity <=4096 each.
  std::uint64_t epoch{};
};
enum class SurfaceReimportPhase : std::uint8_t {
  Idle,Revalidating,ReadingSource,Building,Publishing,Capturing,Stopping,Complete
};
struct SurfaceReimportProgress {
  SurfaceReimportPhase phase{SurfaceReimportPhase::Idle};
  std::size_t source_bytes{};
  bool cancellation_requested{};
};
struct SurfaceReimportCheckpoint {
  std::shared_ptr<const ProjectBrowserView> project;
  SurfaceReimportRequest roots;
  surface_checkpoint::Published published;
  SurfaceReimportCheckpoint(std::shared_ptr<const ProjectBrowserView> view,
      SurfaceReimportRequest names,surface_checkpoint::Published result) noexcept
      :project(std::move(view)),roots(std::move(names)),published(std::move(result)){}
};
struct SurfaceReimportCompletion {
  std::uint64_t request{},epoch{};
  assets::AssetId selected{};
  assets::Digest source{};
  Result<void> result;
  bool changed{};
  std::size_t source_bytes{},product_bytes{},retained_bytes{};
  double admission_ms{},revalidate_ms{},source_ms{},build_ms{},publish_ms{},capture_ms{},publication_ms{};
};
// One joined worker owns trusted bounded PPM/BMP/material parsing and all IO.
// Source <=8 MiB (material <=4096), each native product/texture <=8 MiB,
// project view <=256 KiB and selected checkpoint metadata <=512 KiB.
// These are separate capacity limits, not process RSS or a parser sandbox.
class SurfaceReimportTask {
public:
  using RootFactory=ProjectBrowserTask::RootFactory;
private:
  struct Pending;
  const std::thread::id owner_{std::this_thread::get_id()};
  RootFactory roots_;
  std::uint64_t sequence_{};
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit SurfaceReimportTask(RootFactory={});
  ~SurfaceReimportTask();
  SurfaceReimportTask(const SurfaceReimportTask&)=delete;
  SurfaceReimportTask& operator=(const SurfaceReimportTask&)=delete;
  bool busy() const noexcept{return worker_.joinable();}
  Result<std::uint64_t> start(std::shared_ptr<const ProjectBrowserView>,assets::AssetId,SurfaceReimportRequest);
  Result<void> cancel(); // Sticky; retained IO is not released before physical join.
  Result<SurfaceReimportProgress> progress() const noexcept;
  Result<std::optional<SurfaceReimportCompletion>> poll(std::shared_ptr<const ProjectBrowserView> current,
      assets::AssetId selected,std::uint64_t epoch,std::shared_ptr<const SurfaceReimportCheckpoint>& checkpoint);
};
} // namespace race::desktop
