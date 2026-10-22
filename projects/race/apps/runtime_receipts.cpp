#include "runtime_profile.hpp"

namespace race_runtime {
void receipt_identity(std::ostream &out, const Options &options, const Observations &observed,
                      const race::render::ViewportStats &stats, const race::render::RenderFrame &frame) {
  out << std::boolalpha << std::setprecision(17)
      << "{\n  \"schema\": 1,\n  \"application\": \"race_runtime_desktop\",\n"
      << "  \"source_commit\": " << json_string(race::build::source_commit) << ",\n"
      << "  \"authored_input_sha256\": " << json_string(race::build::authored_input_sha256) << ",\n"
      << "  \"scene_source\": "
      << json_string(!options.package.empty() ? utf8(options.package)
                     : options.scene.empty()  ? "built-in shared Document defaults"
                                              : utf8(options.scene))
      << ",\n"
      << "  \"scene_source_kind\": "
      << json_string(options.package.empty() ? (options.scene.empty() ? "default" : "file") : "package")
      << ",\n"
      << "  \"package_scene_id\": " << json_string(observed.package_scene_id)
      << ",\n  \"package_scene_revision\": " << json_string(observed.package_scene_revision) << ",\n"
      << "  \"scene_load_ms\": " << observed.scene_load_ms << ",\n"
      << "  \"sdl_version\": " << SDL_GetVersion() << ",\n  \"graphics_api\": \"D3D12\",\n"
      << "  \"adapter\": " << json_string(stats.adapter) << ",\n  \"hardware\": " << stats.hardware << ",\n"
      << "  \"vendor_id\": " << stats.vendor_id << ",\n  \"device_id\": " << stats.device_id << ",\n"
      << "  \"dedicated_video_bytes\": " << stats.dedicated_video_bytes << ",\n"
      << "  \"initial_width\": " << observed.initial_width
      << ",\n  \"initial_height\": " << observed.initial_height << ",\n"
      << "  \"width\": " << stats.width << ",\n  \"height\": " << stats.height << ",\n"
      << "  \"submitted_frames\": " << stats.submitted_frames
      << ",\n  \"completed_frames\": " << stats.completed_frames << ",\n"
      << "  \"rendered_frames\": " << observed.rendered
      << ",\n  \"simulation_ticks\": " << observed.simulation_ticks << ",\n"
      << "  \"scene_generation\": " << frame.scene_generation
      << ",\n  \"origin_revision\": " << frame.origin_revision << ",\n"
      << "  \"lighting_profile\": " << unsigned(frame.lighting_profile)
      << ",\n  \"physical_output_installed\": " << stats.physical_output_installed << ",\n"
      << "  \"exposure_scale\": " << stats.exposure_scale
      << ",\n  \"reference_luminance\": " << stats.reference_luminance << ",\n"
      << "  \"scene_bytes\": " << observed.scene_bytes
      << ",\n  \"canonical_scene_roundtrip_equal\": " << observed.parity << ",\n"
      << "  \"authored_scene_unchanged_by_play\": " << observed.edit_unchanged << ",\n"
      << "  \"resize_observed\": " << observed.resized << ",\n  \"minimize_observed\": " << observed.minimized
      << ",\n"
      << "  \"restore_observed\": " << observed.restored
      << ",\n  \"close_event_observed\": " << observed.close_event << ",\n";
}
void receipt_counters(std::ostream &out, const Observations &observed,
                      const race::render::ViewportStats &stats) {
  out << "  \"resize_events\": " << observed.resize_events
      << ",\n  \"minimize_events\": " << observed.minimize_events << ",\n"
      << "  \"restore_events\": " << observed.restore_events
      << ",\n  \"focus_losses\": " << observed.focus_losses << ",\n"
      << "  \"scripted_camera_probe\": " << observed.scripted_camera
      << ",\n  \"maximum_input_dt_seconds\": " << observed.maximum_input_dt << ",\n"
      << "  \"navigation_event_probe\": " << observed.scripted_camera << ",\n"
      << "  \"navigation_scope\": \"Named camera actions sampled at fixed updates; engine clock delivery stamps, separate SDL source time; no replay or device-latency claim\",\n"
      << "  \"input_examined\": " << observed.input_examined
      << ",\n  \"input_delivered\": " << observed.input_delivered << ",\n"
      << "  \"input_ignored\": " << observed.input_ignored
      << ",\n  \"input_stale_devices\": " << observed.input_stale << ",\n"
      << "  \"input_full_batches\": " << observed.input_full_batches
      << ",\n  \"input_budget_updates\": " << observed.input_budget_updates << ",\n"
      << "  \"input_source_stamped\": " << observed.input_source_stamped
      << ",\n  \"input_delivery_clock_domain\": " << observed.input_delivery_domain << ",\n"
      << "  \"input_source_clock_domain\": " << input_api::sdl_event_clock_domain << ",\n"
      << "  \"elapsed_ms\": " << observed.elapsed_ms
      << ",\n  \"gpu_timing_available\": " << stats.gpu_timing_available << ",\n"
      << "  \"last_gpu_ms\": " << stats.gpu_milliseconds << ",\n  \"debug_layer\": " << stats.debug_layer
      << ",\n"
      << "  \"debug_warnings\": " << stats.debug_warnings << ",\n  \"debug_errors\": " << stats.debug_errors
      << ",\n"
      << "  \"device_lost\": " << stats.device_lost
      << ",\n  \"last_native_error\": " << stats.last_native_error << ",\n"
      << "  \"readback\": \"initial.ppm and runtime.ppm; explicit synchronous GPU fence/readback\",\n"
      << "  \"render_scope\": \"opaque unit-cube Raster, directional Lambert and constant ambient; no production sky, PBR, shadows, GI, RT or PT\",\n"
      << "  \"lighting_scope\": \"LegacyViewportV1 unitless defaults, or PhysicalDiffuseV1 photometric lux and cd/m^2 from the Scene RenderOutput and DirectionalLight components with SDR display output only\",\n"
      << "  \"objects\": [";
}
void receipt_objects(std::ostream &out, const race::render::RenderFrame &frame) {
  for (std::size_t i = 0; i < frame.draws.size(); ++i) {
    const auto &draw = frame.draws[i];
    if (i)
      out << ',';
    out << "\n    {\"id_high\": " << draw.id.high << ", \"id_low\": " << draw.id.low << ", \"world\": [";
    for (std::size_t j = 0; j < draw.world.size(); ++j) {
      if (j)
        out << ',';
      out << draw.world[j];
    }
    out << "], \"color\": [" << draw.color[0] << ',' << draw.color[1] << ',' << draw.color[2] << "]}";
  }
}

void receipt_navigation(std::ostream &out, const Observations &observed) {
  out << "\n  ],\n  \"navigation_wait_iterations\": " << observed.navigation_wait_iterations
      << ",\n  \"navigation_external_events\": " << observed.navigation_external_events
      << ",\n  \"navigation_acknowledgments\": [";
  for (std::size_t i = 0; i < observed.navigation_acks.size(); ++i) {
    const auto &ack = observed.navigation_acks[i];
    if (i)
      out << ',';
    out << "\n    {\"source_token\":" << ack.source_token << ",\"sequence\":" << ack.sequence
        << ",\"delivered_ns\":" << ack.delivered_ns << ",\"delivery_tick\":" << ack.delivery_tick
        << ",\"sampled_tick\":" << ack.sampled_tick << ",\"context\":" << ack.context << '}';
  }
}

void save_receipt(const Options &options, const Observations &observed,
                  const race::render::ViewportStats &stats, const race::render::RenderFrame &frame) {
  if (!std::isfinite(stats.gpu_milliseconds) || !std::isfinite(observed.elapsed_ms))
    throw std::runtime_error("non-finite diagnostic timing");
  std::ofstream out(options.smoke / "runtime-receipt.json", std::ios::binary);
  if (!out)
    throw std::runtime_error("cannot open runtime receipt");
  receipt_identity(out, options, observed, stats, frame);
  receipt_counters(out, observed, stats);
  receipt_objects(out, frame);
  receipt_navigation(out, observed);
  out << "\n  ]\n}\n";
  out.flush();
  if (!out)
    throw std::runtime_error("runtime receipt write failed");
}

} // namespace race_runtime
