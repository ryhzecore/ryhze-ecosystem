#include "runtime_loop.hpp"

namespace race_runtime {
void RuntimeLoop::reset_after_device_change() {
  cancel_navigation(false);
  previous = Clock::now();
  previous_present = previous;
  accumulator = 0;
}
race::Result<void> RuntimeLoop::recover_render_failure(race::Result<void> failed) {
  if (failed || viewport->state() != race::render::ViewportState::Removed)
    return failed;
  if (profiling)
    return race::fail(race::ErrorCode::FailedPrerequisite,
                      "profile interrupted by device removal; samples are invalid");
  cancel_navigation(false);
  accumulator = 0;
  auto recovered = viewport->recover_removed(hwnd);
  reset_after_device_change();
  return recovered; // One attempt is enforced by the stable native owner.
}
void RuntimeLoop::resize_viewport(std::uint32_t width, std::uint32_t height) {
  auto resized = viewport->resize(width, height);
  if (!resized && viewport->state() == race::render::ViewportState::Removed) {
    resized = recover_render_failure(resized);
    if (resized)
      resized = viewport->resize(width, height);
  }
  check(resized);
}
void RuntimeLoop::sync_size() {
  int width{}, height{};
  sdl_check(SDL_GetWindowSizeInPixels(window.get(), &width, &height));
  if (width <= 0 || height <= 0)
    return;
  if (profiling && (width != 1280 || height != 720))
    throw std::runtime_error("profile drawable size changed");
  if (width > static_cast<int>(race::render::viewport_max_dimension) ||
      height > static_cast<int>(race::render::viewport_max_dimension))
    throw std::runtime_error("window drawable exceeds the 4096-pixel backend dimension budget");
  resize_viewport(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  observed.resized = observed.resized || static_cast<std::uint32_t>(width) != observed.initial_width ||
                     static_cast<std::uint32_t>(height) != observed.initial_height;
}
bool RuntimeLoop::prepare_frame(Clock::time_point now, bool &focused) {
  if (!options.smoke.empty() && minimize_requested && !restore_requested &&
      now - minimize_time >= std::chrono::milliseconds(120)) {
    sdl_check(SDL_RestoreWindow(window.get()));
    sdl_check(SDL_RaiseWindow(window.get()));
    restore_requested = true;
  }
  // SDL owns window state; keep suspension correct even if a platform coalesces
  // a lifecycle event. The receipt separates observed flags from event counts.
  const bool actual_minimized = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) != 0;
  if (actual_minimized && !minimized) {
    minimized = true;
    observed.minimized = true;
    resize_viewport(0, 0);
    accumulator = 0;
    cancel_navigation(false);
  }
  if (!actual_minimized && minimized) {
    minimized = false;
    observed.restored = observed.minimized;
    sync_size();
  }
  if (profiling && minimized)
    throw std::runtime_error("profile interrupted by minimize; workload invalid");
  if (profiling) {
    int width{}, height{};
    sdl_check(SDL_GetWindowSizeInPixels(window.get(), &width, &height));
    if (width != 1280 || height != 720)
      throw std::runtime_error("profile actual drawable size changed");
  }
  focused = !profiling && !minimized && SDL_GetKeyboardFocus() == window.get();
  if (focused != navigation_focused) {
    if (focused)
      reset_navigation(true);
    else
      cancel_navigation(false);
  }
  if (!options.smoke.empty() && navigation_probe > 0 && navigation_probe < 7 && !focused)
    throw std::runtime_error("actual focus interrupted smoke navigation: " + smoke_state());
  if (minimized) {
    SDL_Delay(8);
    return false;
  }
  if (!options.smoke.empty() && close_requested) {
    ++observed.navigation_wait_iterations;
    SDL_Delay(1);
    return false;
  }
  if (!options.smoke.empty() && observed.rendered >= 6 && navigation_probe == 0 &&
      (!observed.restored || !focused)) {
    // Raising a window is a request, not proof of focus. Keep polling actual
    // lifecycle/input while preserving the remaining fixed render budget.
    if (now - start > std::chrono::seconds{5})
      throw std::runtime_error("smoke navigation readiness deadline: " + smoke_state());
    accumulator = 0;
    SDL_Delay(8);
    return false;
  }
  return true;
}

} // namespace race_runtime
