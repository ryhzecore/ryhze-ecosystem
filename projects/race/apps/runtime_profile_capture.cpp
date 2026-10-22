#include "runtime_loop.hpp"

namespace race_runtime {
void RuntimeLoop::profile_frame(Clock::time_point now,
                                const race::desktop::profile::AllocationSnapshot &allocation_begin,
                                Clock::time_point simulation_start, unsigned steps,
                                Clock::time_point extraction_start, Clock::time_point submit_start,
                                Clock::time_point present_done) {
  if (observed.rendered == 1)
    profile.first_present_ms = milliseconds(application_start, present_done);
  const auto stats = viewport->stats();
  if (stats.last_cpu_frame != observed.rendered || stats.submitted_frames != observed.rendered ||
      stats.width != 1280 || stats.height != 720 || stats.minimized || steps != 1 ||
      observed.simulation_ticks != observed.rendered || last_frame.draws.size() != expected_draws ||
      stats.last_draw_count != expected_draws)
    throw std::runtime_error("profile submission/dimensions/fixed-work invariant failed");
  profile.collect_gpu(stats);
  auto &sample = profile.frames[static_cast<std::size_t>(observed.rendered - 1)];
  sample.simulation_ms = milliseconds(simulation_start, extraction_start);
  sample.extraction_ms = milliseconds(extraction_start, submit_start);
  sample.submit_ms = milliseconds(submit_start, present_done);
  sample.interval_ms = milliseconds(previous_present, present_done);
  previous_present = present_done;
  sample.tick = observed.simulation_ticks;
  sample.completed_at_observation = stats.completed_frames;
  sample.backend_frame = stats.last_cpu_frame;
  sample.submitted_frame = stats.submitted_frames;
  sample.width = stats.width;
  sample.height = stats.height;
  sample.wait_pending = stats.last_inflight_wait_pending;
  sample.backend_ms = {
      stats.last_validation_cpu_ms, stats.last_inflight_wait_cpu_ms, stats.last_pending_fence_cpu_ms,
      stats.last_record_cpu_ms,     stats.last_execute_cpu_ms,       stats.last_present_cpu_ms,
      stats.last_signal_cpu_ms,     stats.last_completion_cpu_ms,    stats.last_draw_cpu_ms};
  if (observed.rendered == 1 || observed.rendered % 60 == 0 || observed.rendered == options.frames) {
    auto memory = race::platform::memory_info();
    if (memory) {
      sample.memory_sampled = true;
      sample.resident_bytes = memory.value().resident_bytes;
      sample.peak_resident_bytes = memory.value().peak_resident_bytes;
    }
  }
  const auto allocation_end = race::desktop::profile::allocation_snapshot();
  sample.allocation_overflow = allocation_begin.overflowed || allocation_end.overflowed;
  if (!sample.allocation_overflow) {
    sample.allocation_requests = allocation_end.requests - allocation_begin.requests;
    sample.allocation_bytes = allocation_end.requested_bytes - allocation_begin.requested_bytes;
  }
  sample.frame_work_ms = milliseconds(now, Clock::now());
  if (observed.rendered == 1)
    check(viewport->capture(options.profile / "profile-initial.ppm"));
}

void RuntimeLoop::finish_profile() {
  if (observed.rendered != options.frames || !observed.close_event)
    throw std::runtime_error("profile interrupted before requested frame count");
  profile.total_ms = milliseconds(application_start, Clock::now());
  allocation_scope.stop();
  // Diagnostic drain/capture is deliberately outside every measured interval.
  check(viewport->capture(options.profile / "profile-final.ppm"));
  const auto stats = viewport->stats();
  profile.collect_gpu(stats);
  const bool complete_gpu = std::all_of(profile.frames.begin(), profile.frames.end(),
                                        [](const auto &frame) { return frame.gpu_valid; });
  const bool allocation_overflow = std::any_of(profile.frames.begin(), profile.frames.end(),
                                               [](const auto &frame) { return frame.allocation_overflow; });
  profile.workload_verified =
      stats.submitted_frames == options.frames && stats.completed_frames == options.frames &&
      observed.simulation_ticks == options.frames && stats.width == 1280 && stats.height == 720 &&
      !stats.minimized && !stats.device_lost && !stats.debug_errors && !allocation_overflow &&
      (!stats.gpu_timing_available || complete_gpu);
  save_profile(options, profile, stats, last_frame.draws.size());
  if (!profile.workload_verified)
    throw std::runtime_error("profile completion/query/workload checks failed; see summary");
  std::cout << "Runtime profile complete: " << profile_warmup << " warmup + "
            << options.frames - profile_warmup << " measured frames; "
            << utf8(options.profile / "profile-summary.json") << '\n';
}

} // namespace race_runtime
