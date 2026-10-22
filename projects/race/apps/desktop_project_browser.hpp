#pragma once
#include "content_project.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace race::desktop {
struct ProjectBrowserRequest {
  // Retained capacities are bounded too: 4096/root and 96/canonical JSON leaf.
  std::string recipe_root,project_leaf;
  std::uint64_t epoch{};
};
struct ProjectBrowserView {
  // The complete view (snapshot, wrapper and retained path capacities) stays
  // inside content_project::maximum_retained_bytes; source bounds are separate too.
  ProjectBrowserRequest source;
  content_project::Snapshot project;
  ProjectBrowserView(ProjectBrowserRequest names,content_project::Snapshot snapshot) noexcept
      :source(std::move(names)),project(std::move(snapshot)){}
};
struct ProjectBrowserCompletion {
  std::uint64_t request{},epoch{};
  Result<void> result;
  bool changed{};
  double admission_ms{},read_ms{},publication_ms{};
  std::size_t entries{},retained_bytes{}; // Whole visible View, including paths.
};
// One owner, one physically joined worker and one terminal completion. Opening
// and refreshing are explicit: browsing never edits Scene or starts imports.
// Advance the epoch for obsolete project context; cancel explicitly for ABA.
class ProjectBrowserTask {
public:
  using RootFactory=std::function<Result<std::shared_ptr<platform::RootedFiles>>(std::string_view)>;
private:
  struct Pending;
  const std::thread::id owner_{std::this_thread::get_id()};
  RootFactory roots_;
  std::uint64_t sequence_{};
  bool cancelled_{};
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit ProjectBrowserTask(RootFactory={});
  ~ProjectBrowserTask();
  ProjectBrowserTask(const ProjectBrowserTask&)=delete;
  ProjectBrowserTask& operator=(const ProjectBrowserTask&)=delete;
  bool busy() const noexcept{return worker_.joinable();} // Owner observation.
  Result<std::uint64_t> start(ProjectBrowserRequest);
  // Sticky publication cancellation; a blocked read remains owned until join.
  Result<void> cancel();
  // Complete closure/path publication is one no-throw owner-side pointer swap.
  Result<std::optional<ProjectBrowserCompletion>> poll(std::uint64_t current_epoch,
      std::shared_ptr<const ProjectBrowserView>& visible);
};
} // namespace race::desktop
