#include "runtime_loop.hpp"

namespace race_runtime {
int RuntimeSession::loop(std::shared_ptr<const race::desktop::MeshSet> &meshes, Observations &observed,
                         std::vector<std::byte> &authored_bytes,
                         std::unique_ptr<SDL_Window, WindowDelete> &window, HWND hwnd,
                         std::unique_ptr<race::render::WindowsViewport> &viewport,
                         const std::size_t expected_draws) {
  Camera camera;
  if (profiling) {
    camera.distance = 50;
    camera.pitch = .60;
  }
  race::render::RenderFrame last_frame;
  const auto start = Clock::now();
  auto previous = start;
  auto previous_present = start;
  auto minimize_time = start;
  double accumulator{};
  bool running = true, dragging = false, minimized = false;
  auto input_events = take(input_api::make_sdl_events(window.get(), *services.clock, 1));
  auto input_batch = std::make_unique<std::array<input_api::Event, input_api::batch_capacity>>();
  race::desktop::NavigationInput navigation;
  bool navigation_focused = !profiling && SDL_GetKeyboardFocus() == window.get();
  check(navigation.reset(services.clock->now().nanoseconds, navigation_focused));
  unsigned navigation_probe{};
  race::scene::Vec3 navigation_before{}, navigation_after{};
  bool resize_requested = false, minimize_requested = false, restore_requested = false,
       close_requested = false;
  const auto window_id = SDL_GetWindowID(window.get());
  RuntimeLoop state{*this,
                    meshes,
                    observed,
                    authored_bytes,
                    window,
                    hwnd,
                    viewport,
                    expected_draws,
                    camera,
                    last_frame,
                    start,
                    previous,
                    previous_present,
                    minimize_time,
                    accumulator,
                    running,
                    dragging,
                    minimized,
                    input_events,
                    input_batch,
                    navigation,
                    navigation_focused,
                    navigation_probe,
                    navigation_before,
                    navigation_after,
                    resize_requested,
                    minimize_requested,
                    restore_requested,
                    close_requested,
                    window_id};
  while (running)
    state.turn();
  check(input_events->close());
  state.reset_navigation(false);
  if (dragging)
    (void)SDL_CaptureMouse(false);
  if (profiling)
    state.finish_profile();
  if (!options.smoke.empty())
    state.finish_smoke();
  else
    check(document->stop_play());
  // Reverse declaration order retires the D3D12 viewport before the SDL window,
  // then SDL itself. No submitted GPU work can outlive its native surface.
  return 0;
}
void RuntimeLoop::turn() {
  const auto now = Clock::now();
  const auto allocation_begin = profiling ? race::desktop::profile::allocation_snapshot()
                                          : race::desktop::profile::AllocationSnapshot{};
  const double wall_dt = std::chrono::duration<double>(now - previous).count();
  previous = now;
  const double input_dt = std::clamp(wall_dt, 0.0, 0.05);
  observed.maximum_input_dt = std::max(observed.maximum_input_dt, input_dt);
  if (options.frames && std::chrono::duration<double>(now - start).count() > (profiling ? 180 : 30))
    throw std::runtime_error(options.smoke.empty()
                                 ? "bounded runtime exceeded its wall-clock watchdog"
                                 : "bounded runtime exceeded its wall-clock watchdog: " + smoke_state());
  poll_input();
  if (!running)
    return;
  bool focused;
  if (!prepare_frame(now, focused))
    return;
  const auto simulation_start = Clock::now();
  // Profiling fixes admitted simulation work rather than varying it with the
  // measured Present cadence; ordinary application/smoke behavior is unchanged.
  const auto steps = simulate(input_dt);
  if (!smoke_navigation())
    return;
  const auto extraction_start = Clock::now();
  last_frame = copy_frame(*document, meshes.get(), camera, observed.simulation_ticks);
  const auto submit_start = Clock::now();
  auto drawn = viewport->draw(last_frame);
  if (!drawn && viewport->state() == race::render::ViewportState::Removed) {
    drawn = recover_render_failure(drawn);
    if (drawn)
      drawn = viewport->draw(last_frame); // No repeated fixed update, input sample or extraction.
  }
  check(drawn);
  const auto present_done = Clock::now();
  runtime_measurement_frame(startup.get(), *viewport, hwnd);
  ++observed.rendered;
  if (profiling)
    profile_frame(now, allocation_begin, simulation_start, steps, extraction_start, submit_start,
                  present_done);
  if (!options.epoch_smoke.empty())
    epoch_smoke();
  smoke_frame(focused);
}
unsigned RuntimeLoop::simulate(double input_dt) {
  accumulator = profiling ? fixed_step : std::min(accumulator + input_dt, fixed_step * 4);
  unsigned steps{};
  while (accumulator >= fixed_step && steps < 4) {
    if (!profiling) {
      const auto input =
          take(navigation.sample(services.clock->now().nanoseconds, observed.simulation_ticks + 1));
      camera.move(input.right, input.forward, input.up, fixed_step);
    }
    check(document->tick_play(fixed_step));
    accumulator -= fixed_step;
    ++steps;
    ++observed.simulation_ticks;
  }
  return steps;
}

} // namespace race_runtime
