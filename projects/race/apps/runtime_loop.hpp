#pragma once
#include "runtime_session.hpp"

namespace race_runtime {
// Nonescaping references replace the original run-local reference-capturing callbacks.
struct RuntimeLoop : RuntimeSession {
  std::shared_ptr<const race::desktop::MeshSet> &meshes;
  Observations &observed;
  std::vector<std::byte> &authored_bytes;
  std::unique_ptr<SDL_Window, WindowDelete> &window;
  HWND &hwnd;
  std::unique_ptr<race::render::WindowsViewport> &viewport;
  const std::size_t &expected_draws;
  Camera &camera;
  race::render::RenderFrame &last_frame;
  const Clock::time_point &start;
  Clock::time_point &previous;
  Clock::time_point &previous_present;
  Clock::time_point &minimize_time;
  double &accumulator;
  bool &running;
  bool &dragging;
  bool &minimized;
  std::unique_ptr<input_api::Events> &input_events;
  std::unique_ptr<std::array<input_api::Event, input_api::batch_capacity>> &input_batch;
  race::desktop::NavigationInput &navigation;
  bool &navigation_focused;
  unsigned &navigation_probe;
  race::scene::Vec3 &navigation_before;
  race::scene::Vec3 &navigation_after;
  bool &resize_requested;
  bool &minimize_requested;
  bool &restore_requested;
  bool &close_requested;
  const SDL_WindowID &window_id;
  void reset_navigation(bool active, std::optional<std::uint64_t> stamp = {});
  void cancel_navigation(bool active, std::optional<std::uint64_t> stamp = {});
  std::optional<std::size_t> probe_index(const input_api::Event &event);
  void acknowledge(std::size_t index, const input_api::Event &event);
  std::string smoke_state();
  void key(std::size_t index, bool down, bool repeat = false);
  void focus(std::size_t index, bool active);
  void reset_after_device_change();
  race::Result<void> recover_render_failure(race::Result<void> failed);
  void resize_viewport(std::uint32_t width, std::uint32_t height);
  void sync_size();
  void poll_input();
  void input_event(const input_api::Event &event);
  void window_event(const input_api::Event &event, std::optional<std::size_t> token);
  void camera_event(const input_api::Event &event);
  void navigation_event(const input_api::Event &event, std::optional<std::size_t> token);
  bool prepare_frame(Clock::time_point now, bool &focused);
  unsigned simulate(double input_dt);
  bool smoke_navigation();
  void profile_frame(Clock::time_point now,
                     const race::desktop::profile::AllocationSnapshot &allocation_begin,
                     Clock::time_point simulation_start, unsigned steps, Clock::time_point extraction_start,
                     Clock::time_point submit_start, Clock::time_point present_done);
  void epoch_smoke();
  void smoke_frame(bool focused);
  void turn();
  void finish_profile();
  void finish_smoke();
};
} // namespace race_runtime
