#pragma once
#include "runtime_common.hpp"
#include "runtime_startup_options.hpp"

namespace race_runtime {
struct ProfileFrame {
  double extraction_ms{}, simulation_ms{}, submit_ms{}, interval_ms{}, frame_work_ms{}, gpu_ms{};
  std::array<double, 9> backend_ms{};
  std::uint64_t allocation_requests{}, allocation_bytes{}, resident_bytes{}, peak_resident_bytes{};
  std::uint64_t tick{}, completed_at_observation{}, backend_frame{}, submitted_frame{};
  std::uint32_t width{}, height{};
  bool gpu_valid{}, memory_sampled{}, allocation_overflow{}, wait_pending{};
};
struct ProfileData {
  std::uint64_t device_epoch{};
  std::vector<ProfileFrame> frames;
  double scene_load_ms{}, document_create_ms{}, first_present_ms{}, total_ms{};
  std::string package_scene_id, package_scene_revision;
  bool allocation_available{}, allocation_active{}, workload_verified{};
  void collect_gpu(const race::render::ViewportStats &stats);
};
void save_profile(const Options &, const ProfileData &, const race::render::ViewportStats &, std::size_t);
void save_receipt(const Options &, const Observations &, const race::render::ViewportStats &,
                  const race::render::RenderFrame &);
} // namespace race_runtime
