#pragma once
#include "runtime_common.hpp"
#include "runtime_profile.hpp"
#include "runtime_startup_options.hpp"
#include "runtime_startup_measurement.hpp"

namespace race_runtime {
// References only. Each owning phase stays on the stack until all later phases finish.
struct RuntimeSession {
  const Options &options;
  const Clock::time_point &application_start;
  const bool &profiling;
  ProfileData &profile;
  std::unique_ptr<race::desktop::startup::Witness> &startup;
  race::desktop::profile::AllocationScope &allocation_scope;
  race::platform::Services &services;
  std::unique_ptr<race::desktop::Document> &document;
  int scene();
  int window(const std::vector<race::desktop::MeshReference> &mesh_refs,
             std::shared_ptr<const race::desktop::MeshSet> &meshes, const std::string &package_scene_id,
             const std::string &package_scene_revision);
  int mesh_smoke(const std::vector<race::desktop::MeshReference> &mesh_refs,
                 std::shared_ptr<const race::desktop::MeshSet> &meshes, const std::string &package_scene_id,
                 const std::string &package_scene_revision,
                 std::unique_ptr<race::render::WindowsViewport> &viewport);
  int viewport(std::shared_ptr<const race::desktop::MeshSet> &meshes, Observations &observed,
               std::vector<std::byte> &authored_bytes, std::unique_ptr<SDL_Window, WindowDelete> &window,
               HWND hwnd, std::unique_ptr<race::render::WindowsViewport> &viewport);
  int loop(std::shared_ptr<const race::desktop::MeshSet> &meshes, Observations &observed,
           std::vector<std::byte> &authored_bytes, std::unique_ptr<SDL_Window, WindowDelete> &window,
           HWND hwnd, std::unique_ptr<race::render::WindowsViewport> &viewport,
           const std::size_t expected_draws);
};
int run(const Options &options);
} // namespace race_runtime
