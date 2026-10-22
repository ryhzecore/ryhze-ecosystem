#include "runtime_session.hpp"

namespace race_runtime {
void size_runtime_viewport(race::desktop::startup::Witness *witness, SDL_Window *window) {
  if (!witness) {
    sdl_check(SDL_SetWindowMaximumSize(window, 4096, 4096));
    return;
  }
  sdl_check(SDL_SetWindowMinimumSize(window, 1280, 720));
  sdl_check(SDL_SetWindowMaximumSize(window, 1280, 720));
  int width{}, height{};
  sdl_check(SDL_GetWindowSizeInPixels(window, &width, &height));
  if (width != 1280 || height != 720)
    throw std::runtime_error("startup measurement requires a 1280x720 client viewport");
}
void initialize_runtime_profile(const Options &options, ProfileData &profile) {
  if (!options.profile.empty()) {
    if (fs::exists(options.profile))
      throw std::runtime_error("profile needs a fresh output directory");
    fs::create_directories(options.profile);
    profile.frames.resize(static_cast<std::size_t>(options.frames));
    profile.allocation_available = race::desktop::profile::allocation_counting_available();
  }
}
std::unique_ptr<race::desktop::startup::Witness> runtime_measurement(const Options &options) {
  if (options.startup.empty())
    return {};
  if (options.scene.empty() || !options.smoke.empty() || !options.profile.empty() ||
      !options.package.empty() || !options.assets.empty() || !options.mesh_smoke.empty() ||
      !options.epoch_smoke.empty() || options.frames)
    throw std::runtime_error("startup measurement requires only empty Scene and shaders");
  return std::make_unique<race::desktop::startup::Witness>(options.startup, "runtime");
}
void runtime_measurement_frame(race::desktop::startup::Witness *witness,
                               race::render::WindowsViewport &viewport, HWND window) {
  if (!witness)
    return;
  witness->visible(window);
  const auto &s = viewport.stats();
  if (!s.has_image || s.minimized || s.diagnostics_incomplete || s.recreation_attempts ||
      s.removal_recovery_attempts)
    throw std::runtime_error("startup viewport image/lifecycle diagnostics invalid");
  witness->frame(static_cast<std::size_t>(s.last_draw_count), s.width, s.height, s.device_epoch, s.hardware,
                 s.debug_errors, s.debug_warnings, s.adapter);
}

} // namespace race_runtime
