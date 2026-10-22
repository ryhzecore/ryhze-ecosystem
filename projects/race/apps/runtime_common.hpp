#pragma once
#include <array>
#include <vector>
#include "startup_measurement.hpp"
#include "desktop_scene.hpp"
#include "desktop_meshes.hpp"
#include "desktop_package.hpp"
#include "desktop_profile.hpp"
#include "desktop_navigation.hpp"
#include "platform/sdl_input.hpp"
#include "race/build_identity.hpp"
#include "../src/render/windows_viewport.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace race_runtime {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
namespace input_api = race::platform::input;
constexpr std::size_t scene_byte_limit = 64U * 1024U * 1024U;
constexpr double fixed_step = 1.0 / 60.0;

template <class T> T take(race::Result<T> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
  return std::move(result).value();
}
inline void check(race::Result<void> result) {
  if (!result)
    throw std::runtime_error(result.error().message);
}
inline void sdl_check(bool success) {
  if (!success)
    throw std::runtime_error(SDL_GetError());
}
fs::path from_utf8(std::string_view value);
std::string utf8(const fs::path &value);
std::string json_string(std::string_view value);
struct SdlLifetime {
  SdlLifetime() { sdl_check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)); }
  ~SdlLifetime() { SDL_Quit(); }
};
struct WindowDelete {
  void operator()(SDL_Window *window) const { SDL_DestroyWindow(window); }
};
// Application and RenderFrame use double-precision absolute Scene metres.
// The backend converts world/camera to its declared camera-relative GPU frame.
struct Camera {
  race::scene::Vec3 target{0, 0.6, 0};
  double yaw{0.64}, pitch{0.40}, distance{11};
  void orbit(double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy))
      return;
    yaw = std::remainder(yaw - std::clamp(dx, -200.0, 200.0) * 0.005, 6.283185307179586);
    pitch = std::clamp(pitch + std::clamp(dy, -200.0, 200.0) * 0.005, -1.45, 1.45);
  }
  void zoom(double amount) {
    if (std::isfinite(amount))
      distance = std::clamp(distance * std::exp(-std::clamp(amount, -8.0, 8.0) * 0.12), 0.5, 500.0);
  }
  void move(double right, double forward, double vertical, double seconds) {
    const double length = std::sqrt(right * right + forward * forward + vertical * vertical);
    if (length == 0)
      return;
    const double scale = 6 * std::clamp(seconds, 0.0, 0.05) / std::max(1.0, length);
    target.x = std::clamp(target.x + (right * std::cos(yaw) - forward * std::sin(yaw)) * scale, -1000000.0,
                          1000000.0);
    target.y = std::clamp(target.y + vertical * scale, -1000000.0, 1000000.0);
    target.z = std::clamp(target.z + (-right * std::sin(yaw) - forward * std::cos(yaw)) * scale, -1000000.0,
                          1000000.0);
  }
  race::render::ViewportCamera view() const {
    race::render::ViewportCamera result;
    result.target = {target.x, target.y, target.z};
    result.eye = {result.target.x + distance * std::cos(pitch) * std::sin(yaw),
                  result.target.y + distance * std::sin(pitch),
                  result.target.z + distance * std::cos(pitch) * std::cos(yaw)};
    return result;
  }
};
race::render::RenderFrame copy_frame(race::desktop::Document &, const race::desktop::MeshSet *,
                                     const Camera &, std::uint64_t);
// Opaque smoke identities carried in SDL source time, never used as delivery time.
constexpr std::uint64_t navigation_probe_token = 0x5241434500000000ULL;
struct NavigationProbeAck {
  std::uint64_t source_token{}, sequence{}, delivered_ns{}, delivery_tick{}, sampled_tick{}, context{};
};
struct Observations {
  std::uint64_t rendered{}, simulation_ticks{}, resize_events{}, minimize_events{}, restore_events{},
      focus_losses{};
  std::uint64_t input_examined{}, input_delivered{}, input_ignored{}, input_stale{}, input_full_batches{},
      input_budget_updates{}, input_source_stamped{}, input_delivery_domain{};
  std::array<NavigationProbeAck, 8> navigation_acks{};
  std::uint64_t navigation_wait_iterations{}, navigation_external_events{};
  std::uint32_t initial_width{}, initial_height{};
  bool resized{}, minimized{}, restored{}, close_event{}, parity{}, edit_unchanged{}, scripted_camera{};
  std::size_t scene_bytes{};
  double elapsed_ms{}, maximum_input_dt{}, scene_load_ms{};
  std::string package_scene_id, package_scene_revision;
};
constexpr std::size_t profile_warmup = 60;
constexpr std::array<const char *, 9> backend_columns{
    "validation_cpu_ms", "inflight_wait_cpu_ms", "pending_fence_cpu_ms", "record_cpu_ms", "execute_cpu_ms",
    "present_cpu_ms",    "signal_cpu_ms",        "completion_cpu_ms",    "draw_cpu_ms"};
inline double milliseconds(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace race_runtime
