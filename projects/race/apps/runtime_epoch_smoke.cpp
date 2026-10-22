#include "runtime_loop.hpp"

namespace race_runtime {
void RuntimeLoop::epoch_smoke() {
  const auto before = viewport->stats();
  const auto camera_before = camera;
  const auto generation = document->generation(true), tick = observed.simulation_ticks;
  auto image = take(viewport->image_token());
  check(viewport->capture(options.epoch_smoke / "before.ppm"));
  check(document->cook(*services.files, utf8(options.epoch_smoke / "authoring-before.rscene")));
  reset_navigation(true);
  dragging = true;
  check(navigation.push({navigation.context(), services.clock->now().nanoseconds,
                         race::desktop::NavigationAction::Right, true, false}));
  check(viewport->retire_surface());
  check(viewport->recreate(hwnd));
  reset_after_device_change();
  const auto cancelled = take(navigation.sample(services.clock->now().nanoseconds, tick + 1));
  const auto stale = viewport->capture(options.epoch_smoke / "stale.ppm", image);
  check(viewport->draw(last_frame)); // Existing owned extraction; no input sample or tick repeated.
  check(viewport->capture(options.epoch_smoke / "restored.ppm"));
  check(document->cook(*services.files, utf8(options.epoch_smoke / "authoring-after.rscene")));
  const auto after = viewport->stats();
  const auto same = [&](const char *a, const char *b) {
    return take(services.files->read(utf8(options.epoch_smoke / a), scene_byte_limit)) ==
           take(services.files->read(utf8(options.epoch_smoke / b), scene_byte_limit));
  };
  const bool preserved = document->playing() && document->generation(true) == generation &&
                         observed.simulation_ticks == tick &&
                         after.simulation_tick == last_frame.simulation_tick &&
                         after.scene_generation == last_frame.scene_generation &&
                         camera.yaw == camera_before.yaw && camera.pitch == camera_before.pitch &&
                         camera.distance == camera_before.distance && camera.target == camera_before.target;
  const bool input_cancelled = !dragging && !navigation_focused && navigation.pending() == 0 &&
                               cancelled.right == 0 && cancelled.forward == 0 && cancelled.up == 0 &&
                               accumulator == 0;
  const bool pixels = same("before.ppm", "restored.ppm"),
             authoring = same("authoring-before.rscene", "authoring-after.rscene");
  const bool stale_rejected = !stale && stale.error().code == race::ErrorCode::StaleHandle &&
                              !fs::exists(options.epoch_smoke / "stale.ppm");
  const bool passed = preserved && input_cancelled && pixels && authoring && stale_rejected &&
                      after.device_epoch == before.device_epoch + 1 && after.hardware && after.debug_layer &&
                      !after.debug_errors && !after.debug_warnings && !after.diagnostics_incomplete &&
                      after.retired.completed_frames == after.retired.submitted_frames &&
                      after.completed_frames == after.submitted_frames;
  std::ofstream out(options.epoch_smoke / "epoch-receipt.json", std::ios::binary);
  out << std::boolalpha;
  out << "{\"schema\":1,\"application\":\"RACE SDL epoch smoke\",\"passed\":" << passed
      << ",\"source_commit\":" << json_string(race::build::source_commit)
      << ",\"authored_input_sha256\":" << json_string(race::build::authored_input_sha256)
      << ",\"authority_preserved\":" << preserved << ",\"queued_navigation_cancelled\":" << input_cancelled
      << ",\"pixels_equal\":" << pixels << ",\"authoring_equal\":" << authoring
      << ",\"stale_capture_rejected\":" << stale_rejected
      << ",\"lighting_profile\":" << unsigned(after.lighting_profile)
      << ",\"physical_output_installed\":" << after.physical_output_installed
      << ",\"initial_epoch\":" << before.device_epoch << ",\"final_epoch\":" << after.device_epoch
      << ",\"same_tick\":" << tick << ",\"debug_errors\":" << after.debug_errors
      << ",\"debug_warnings\":" << after.debug_warnings
      << ",\"scope\":\"Controlled SDL viewport recreation and neutral queued-input cancellation; no native device-removal or physical-keyboard claim\"}\n";
  out.close();
  if (!out || !passed)
    throw std::runtime_error("SDL epoch composition checks failed; see retained receipt");
}

} // namespace race_runtime
