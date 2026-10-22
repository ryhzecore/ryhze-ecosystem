#include "runtime_loop.hpp"

namespace race_runtime {
std::optional<race::desktop::NavigationAction> movement(input_api::Key code) {
  using A = race::desktop::NavigationAction;
  switch (code) {
  case input_api::Key::A:
    return A::Left;
  case input_api::Key::D:
    return A::Right;
  case input_api::Key::W:
    return A::Forward;
  case input_api::Key::S:
    return A::Backward;
  case input_api::Key::Q:
    return A::Down;
  case input_api::Key::E:
    return A::Up;
  default:
    return {};
  }
}
void RuntimeLoop::reset_navigation(bool active, std::optional<std::uint64_t> stamp) {
  navigation_focused = active;
  check(navigation.reset(stamp.value_or(services.clock->now().nanoseconds), active));
}
void RuntimeLoop::cancel_navigation(bool active, std::optional<std::uint64_t> stamp) {
  dragging = false;
  (void)SDL_CaptureMouse(false);
  reset_navigation(active, stamp);
}
std::optional<std::size_t> RuntimeLoop::probe_index(const input_api::Event &event) {
  if (options.smoke.empty() || !event.source_time ||
      event.source_time->domain != input_api::sdl_event_clock_domain)
    return {};
  const auto token = event.source_time->nanoseconds;
  if (token <= navigation_probe_token || token > navigation_probe_token + observed.navigation_acks.size())
    return {};
  return static_cast<std::size_t>(token - navigation_probe_token - 1);
}
void RuntimeLoop::acknowledge(std::size_t index, const input_api::Event &event) {
  auto &ack = observed.navigation_acks[index];
  if (ack.sequence)
    throw std::runtime_error("duplicate smoke navigation event");
  ack = {event.source_time->nanoseconds,
         event.sequence,
         event.delivered.nanoseconds,
         observed.simulation_ticks,
         0,
         navigation.context()};
  if (index == 0 || index == 6)
    navigation_before = camera.target;
  if (index == 1 || index == 3 || index == 5 || index == 7)
    navigation_after = camera.target;
}
void RuntimeLoop::poll_input() {
  std::size_t input_examined{};
  // The service bounds records without claiming a wall-clock bound for SDL.
  while (input_examined < input_api::work_capacity) {
    auto batch = take(input_events->poll(*input_batch, input_api::work_capacity - input_examined));
    input_examined += batch.examined;
    observed.input_examined += batch.examined;
    observed.input_delivered += batch.written;
    observed.input_ignored += batch.ignored;
    observed.input_stale += batch.stale_devices;
    if (batch.output_full)
      ++observed.input_full_batches;
    if (batch.terminal) {
      cancel_navigation(false);
      throw std::runtime_error(batch.terminal->message);
    }
    for (std::size_t i = 0; i < batch.written; ++i) {
      input_event((*input_batch)[i]);
    }
    if (batch.observed_empty || !running)
      break;
  }
  if (startup && input_examined < input_api::work_capacity)
    startup->turn();
  if (input_examined == input_api::work_capacity)
    ++observed.input_budget_updates;
}
void RuntimeLoop::input_event(const input_api::Event &event) {
  using K = input_api::Kind;
  observed.input_delivery_domain = event.delivered.domain;
  if (event.source_time)
    ++observed.input_source_stamped;
  const auto token = probe_index(event);
  if (navigation_probe > 0 && navigation_probe < 7 && !token &&
      ((event.kind == K::Key && movement(event.key)) || event.kind == K::FocusLost ||
       event.kind == K::FocusGained || event.kind == K::DeviceRemoved || event.kind == K::Minimized ||
       event.kind == K::RightButton || event.kind == K::Wheel)) {
    ++observed.navigation_external_events;
    throw std::runtime_error("unscripted input interrupted smoke navigation: kind=" +
                             std::to_string(static_cast<unsigned>(event.kind)) +
                             ", key=" + std::to_string(static_cast<unsigned>(event.key)) +
                             ", sequence=" + std::to_string(event.sequence) + ", provenance=" +
                             std::to_string(static_cast<unsigned>(event.device.provenance)) + ", " +
                             smoke_state());
  }
  window_event(event, token);
  camera_event(event);
  navigation_event(event, token);
}
void RuntimeLoop::window_event(const input_api::Event &event, std::optional<std::size_t> token) {
  using K = input_api::Kind;
  const auto cancel = [&](bool active) { cancel_navigation(active, event.delivered.nanoseconds); };
  if (event.kind == K::Quit || event.kind == K::Close) {
    running = false;
    observed.close_event = true;
  }
  if (event.kind == K::FocusLost) {
    ++observed.focus_losses;
    cancel(false);
    if (token && *token == 3 && navigation_probe == 3)
      acknowledge(*token, event);
  }
  if (event.kind == K::FocusGained) {
    reset_navigation(!profiling && !minimized, event.delivered.nanoseconds);
    if (token && *token == 4 && navigation_probe == 4)
      acknowledge(*token, event);
  }
  if (event.kind == K::DeviceRemoved)
    cancel(navigation_focused);
  if (event.kind == K::Minimized) {
    if (profiling)
      throw std::runtime_error("profile interrupted by minimize; workload invalid");
    minimized = true;
    observed.minimized = true;
    ++observed.minimize_events;
    resize_viewport(0, 0);
    accumulator = 0;
    cancel(false);
  }
  if (event.kind == K::Restored) {
    minimized = false;
    observed.restored = observed.minimized;
    ++observed.restore_events;
    sync_size();
  }
  if (event.kind == K::Resized && !minimized) {
    ++observed.resize_events;
    sync_size();
  }
}
void RuntimeLoop::camera_event(const input_api::Event &event) {
  using K = input_api::Kind;
  if (event.kind == K::RightButton) {
    if (!event.pressed) {
      dragging = false;
      (void)SDL_CaptureMouse(false);
    } else if (!profiling && !minimized && navigation_focused) {
      dragging = true;
      sdl_check(SDL_CaptureMouse(true));
    }
  }
  if (!profiling && event.kind == K::Motion && dragging && !minimized && navigation_focused &&
      SDL_GetKeyboardFocus() == window.get() &&
      (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) == 0)
    camera.orbit(event.x, event.y);
  if (!profiling && !minimized && navigation_focused && event.kind == K::Wheel)
    camera.zoom(event.y);
  if (event.kind == K::Key && event.pressed && event.key == input_api::Key::Escape)
    running = false;
}
void RuntimeLoop::navigation_event(const input_api::Event &event, std::optional<std::size_t> token) {
  using K = input_api::Kind;
  if (!profiling && event.kind == K::Key) {
    if (auto action = movement(event.key); action && navigation.active()) {
      if (!event.pressed || (event.modifiers & (input_api::Control | input_api::Alt | input_api::Gui)) == 0) {
        auto admitted = navigation.push(
            {navigation.context(), event.delivered.nanoseconds, *action, event.pressed, event.repeat});
        if (!admitted) {
          std::cerr << admitted.error().message << '\n';
          reset_navigation(navigation_focused, event.delivered.nanoseconds);
        } else if (token && event.key == input_api::Key::D &&
                   event.device.provenance == input_api::Provenance::Unknown &&
                   ((*token == 0 && navigation_probe == 1 && event.pressed && !event.repeat) ||
                    (*token == 1 && navigation_probe == 2 && !event.pressed && !event.repeat) ||
                    (*token == 2 && navigation_probe == 3 && event.pressed && !event.repeat) ||
                    (*token == 5 && navigation_probe == 4 && event.pressed && event.repeat &&
                     observed.navigation_acks[4].sequence) ||
                    (*token == 6 && navigation_probe == 5 && event.pressed && !event.repeat) ||
                    (*token == 7 && navigation_probe == 6 && !event.pressed && !event.repeat)))
          acknowledge(*token, event);
      }
    }
  }
}

} // namespace race_runtime
