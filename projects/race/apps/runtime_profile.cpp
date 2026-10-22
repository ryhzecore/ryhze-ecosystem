#include "runtime_profile.hpp"

namespace race_runtime {
void ProfileData::collect_gpu(const race::render::ViewportStats &stats) {
  if (!device_epoch)
    device_epoch = stats.device_epoch;
  if (!device_epoch || device_epoch != stats.device_epoch ||
      stats.state != race::render::ViewportState::Ready)
    throw std::runtime_error("profile device epoch changed or became unavailable");
  if (stats.gpu_sample_count > stats.gpu_samples.size())
    throw std::runtime_error("invalid GPU timing history count");
  for (std::size_t i = 0; i < stats.gpu_sample_count; ++i) {
    const auto &sample = stats.gpu_samples[i];
    if (sample.device_epoch != device_epoch || !sample.frame || sample.frame > frames.size() ||
        sample.frame > stats.submitted_frames)
      throw std::runtime_error("invalid GPU timing frame ID");
    if (!std::isfinite(sample.milliseconds) || sample.milliseconds < 0)
      throw std::runtime_error("invalid GPU timing sample");
    auto &frame = frames[static_cast<std::size_t>(sample.frame - 1)];
    if (frame.gpu_valid && frame.gpu_ms != sample.milliseconds)
      throw std::runtime_error("GPU timing changed for a completed submission ID");
    frame.gpu_ms = sample.milliseconds;
    frame.gpu_valid = true;
  }
}
void distribution(std::ostream &out, std::vector<double> values) {
  if (values.empty()) {
    out << "null";
    return;
  }
  for (double value : values)
    if (!std::isfinite(value) || value < 0)
      throw std::runtime_error("invalid profile distribution");
  std::sort(values.begin(), values.end());
  const auto percentile = [&](double fraction) {
    const double index = fraction * static_cast<double>(values.size() - 1);
    const auto low = static_cast<std::size_t>(std::floor(index));
    const auto high = static_cast<std::size_t>(std::ceil(index));
    return values[low] + (values[high] - values[low]) * (index - static_cast<double>(low));
  };
  out << "{\"count\":" << values.size() << ",\"median\":" << percentile(.5) << ",\"p95\":" << percentile(.95)
      << ",\"p99\":" << percentile(.99) << ",\"max\":" << values.back() << ",\"spikes_gt_16_67_ms\":"
      << std::count_if(values.begin(), values.end(), [](double value) { return value > 16.67; }) << '}';
}
void profile_csv(std::ostream &csv, const ProfileData &profile) {
  csv << std::setprecision(17)
      << "frame,phase,simulation_tick,extraction_cpu_ms,simulation_cpu_ms,render_submit_cpu_ms,frame_interval_ms,frame_work_ms,gpu_ms,gpu_valid,completed_frames_observed";
  for (const auto *column : backend_columns)
    csv << ',' << column;
  csv << ",backend_cpu_frame,inflight_wait_pending,allocation_requests,allocation_requested_bytes,allocation_overflow,memory_sampled,resident_bytes,peak_resident_bytes,submitted_frame_id,width,height\n";
  for (std::size_t i = 0; i < profile.frames.size(); ++i) {
    const auto &f = profile.frames[i];
    csv << i + 1 << ',' << (i < profile_warmup ? "warmup" : "measure") << ',' << f.tick << ','
        << f.extraction_ms << ',' << f.simulation_ms << ',' << f.submit_ms << ',' << f.interval_ms << ','
        << f.frame_work_ms << ',';
    if (f.gpu_valid)
      csv << f.gpu_ms;
    csv << ',' << f.gpu_valid << ',' << f.completed_at_observation;
    for (double value : f.backend_ms)
      csv << ',' << value;
    csv << ',' << f.backend_frame << ',' << f.wait_pending << ',' << f.allocation_requests << ','
        << f.allocation_bytes << ',' << f.allocation_overflow << ',' << f.memory_sampled << ',';
    if (f.memory_sampled)
      csv << f.resident_bytes;
    csv << ',';
    if (f.memory_sampled)
      csv << f.peak_resident_bytes;
    csv << ',' << f.submitted_frame << ',' << f.width << ',' << f.height << '\n';
  }
  csv.flush();
  if (!csv)
    throw std::runtime_error("profile CSV write failed");
}

void profile_header(std::ostream &out, const Options &options, const ProfileData &profile,
                    const race::render::ViewportStats &stats, std::size_t objects) {
  out << std::boolalpha << std::setprecision(17)
      << "{\n\"schema\":2,\"application\":\"race_runtime_desktop\",\"source_commit\":"
      << json_string(race::build::source_commit)
      << ",\"authored_input_sha256\":" << json_string(race::build::authored_input_sha256)
      << ",\"scene_source\":"
      << json_string(!options.package.empty() ? utf8(options.package)
                     : options.scene.empty()  ? "shared default"
                                              : utf8(options.scene))
      << ",\"package_scene_id\":" << json_string(profile.package_scene_id)
      << ",\"package_scene_revision\":" << json_string(profile.package_scene_revision)
      << ",\"adapter\":" << json_string(stats.adapter) << ",\"hardware\":" << stats.hardware
      << ",\"vendor_id\":" << stats.vendor_id << ",\"device_id\":" << stats.device_id
      << ",\"dedicated_video_bytes\":" << stats.dedicated_video_bytes << ",\"width\":" << stats.width
      << ",\"height\":" << stats.height << ",\"objects\":" << objects
      << ",\"submitted_frames\":" << stats.submitted_frames
      << ",\"completed_frames\":" << stats.completed_frames
      << ",\"expected_submitted_frames\":" << profile.frames.size()
      << ",\"workload_verified\":" << profile.workload_verified << ",\"warmup_frames\":" << profile_warmup
      << ",\"measured_frames\":" << profile.frames.size() - profile_warmup
      << ",\"native_resolution\":true,\"antialiasing\":\"none\",\"quality_tier\":\"unimplemented\""
      << ",\"mode\":\"opaque Box Raster, directional Lambert plus constant ambient, grid\""
      << ",\"present_sync_interval\":1,\"render_scale\":1,\"frame_generation\":false"
      << ",\"camera\":\"fixed target(0,.6,0), yaw.64, pitch.60, distance50m\""
      << ",\"simulation\":\"one fixed1/60s sample-Play step per profile frame; first Box rotates; not physics\""
      << ",\"document_create_ms\":" << profile.document_create_ms
      << ",\"scene_load_measured\":" << (!options.scene.empty() || !options.package.empty())
      << ",\"scene_load_ms\":";
  if (options.scene.empty() && options.package.empty())
    out << "null";
  else
    out << profile.scene_load_ms;
  out << ",\"startup_first_present_ms\":" << profile.first_present_ms
      << ",\"profile_total_ms\":" << profile.total_ms
      << ",\"allocation_counting_available\":" << profile.allocation_available
      << ",\"allocation_scope_active\":" << profile.allocation_active
      << ",\"allocation_scope\":\"EXE and linked static-library C++ allocation attempts; excludes Qt/SDL/driver DLL internals and malloc; not live allocations\""
      << ",\"instrumentation\":\"frame timing/query collection and periodic memory observation included; preallocated rows and post-run CSV/summary output\""
      << ",\"memory_scope\":\"process working set/resident and peak resident; private bytes unavailable via Platform API; sampled every60 frames\""
      << ",\"gpu_scope\":\"completed frame-ID-associated timestamp queries; clears/raster/color-copy; excludes Present/fence wait; final capture drains outside measurement\""
      << ",\"cpu_scope\":\"steady-clock wall durations, not CPU utilization; submit includes backend fence/Present waits; frame interval includes preceding event-loop/observer work\""
      << ",\"percentile_method\":\"linear interpolation at p*(n-1)\""
      << ",\"debug_layer\":" << stats.debug_layer << ",\"debug_warnings\":" << stats.debug_warnings
      << ",\"debug_errors\":" << stats.debug_errors << ",\"device_lost\":" << stats.device_lost
      << ",\"gpu_timing_available\":" << stats.gpu_timing_available << ",\"statistics_ms\":{\n";
}

void profile_metrics(std::ostream &out, const ProfileData &profile) {
  bool first = true;
  const auto metric = [&](const char *name, auto getter, bool require_gpu = false) {
    if (!first)
      out << ",\n";
    first = false;
    out << json_string(name) << ':';
    std::vector<double> values;
    values.reserve(profile.frames.size() - profile_warmup);
    for (std::size_t i = profile_warmup; i < profile.frames.size(); ++i)
      if (!require_gpu || profile.frames[i].gpu_valid)
        values.push_back(getter(profile.frames[i]));
    distribution(out, std::move(values));
  };
  metric("extraction_cpu_ms", [](const auto &f) { return f.extraction_ms; });
  metric("simulation_cpu_ms", [](const auto &f) { return f.simulation_ms; });
  metric("render_submit_cpu_ms", [](const auto &f) { return f.submit_ms; });
  metric("frame_interval_ms", [](const auto &f) { return f.interval_ms; });
  metric("frame_work_ms", [](const auto &f) { return f.frame_work_ms; });
  metric("gpu_ms", [](const auto &f) { return f.gpu_ms; }, true);
  for (std::size_t i = 0; i < backend_columns.size(); ++i)
    metric(backend_columns[i], [i](const auto &f) { return f.backend_ms[i]; });
}

void profile_totals(std::ostream &out, const ProfileData &profile) {
  std::uint64_t requests{}, bytes{}, peak_resident{}, resident_min = UINT64_MAX, resident_max{};
  bool overflow{};
  std::size_t memory_samples{}, gpu_samples{};
  const auto all_gpu_samples = static_cast<std::size_t>(std::count_if(
      profile.frames.begin(), profile.frames.end(), [](const auto &frame) { return frame.gpu_valid; }));
  for (std::size_t i = profile_warmup; i < profile.frames.size(); ++i) {
    const auto &f = profile.frames[i];
    requests += f.allocation_requests;
    bytes += f.allocation_bytes;
    overflow = overflow || f.allocation_overflow;
    if (f.gpu_valid)
      ++gpu_samples;
    if (f.memory_sampled) {
      ++memory_samples;
      resident_min = std::min(resident_min, f.resident_bytes);
      resident_max = std::max(resident_max, f.resident_bytes);
      peak_resident = std::max(peak_resident, f.peak_resident_bytes);
    }
  }
  out << "\n},\"gpu_sample_count\":" << all_gpu_samples
      << ",\"gpu_samples_complete\":" << (all_gpu_samples == profile.frames.size())
      << ",\"measured_gpu_samples\":" << gpu_samples << ",\"measured_allocation_requests\":" << requests
      << ",\"measured_allocation_requested_bytes\":" << bytes << ",\"allocation_overflow\":" << overflow
      << ",\"measured_memory_samples\":" << memory_samples
      << ",\"resident_min_bytes\":" << (memory_samples ? resident_min : 0)
      << ",\"resident_max_bytes\":" << resident_max << ",\"peak_resident_bytes\":" << peak_resident
      << ",\"qualification\":\"fixed small-workload measurement only; no8K/RTX5090/production-quality or throughput qualification\"}\n";
}

void save_profile(const Options &options, const ProfileData &profile,
                  const race::render::ViewportStats &stats, std::size_t objects) {
  std::ofstream csv(options.profile / "frames.csv", std::ios::binary);
  profile_csv(csv, profile);
  std::ofstream out(options.profile / "profile-summary.json", std::ios::binary);
  profile_header(out, options, profile, stats, objects);
  profile_metrics(out, profile);
  profile_totals(out, profile);
  out.flush();
  if (!out)
    throw std::runtime_error("profile summary write failed");
}

} // namespace race_runtime
