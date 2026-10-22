#include "runtime_loop.hpp"

namespace race_runtime {
std::string RuntimeLoop::smoke_state() {
  return "rendered=" + std::to_string(observed.rendered) + ", requested=" + std::to_string(options.frames) +
         ", resized=" + std::to_string(observed.resized) +
         ", minimized=" + std::to_string(observed.minimized) +
         ", restored=" + std::to_string(observed.restored) +
         ", close=" + std::to_string(observed.close_event) +
         ", camera=" + std::to_string(observed.scripted_camera) +
         ", navigation_stage=" + std::to_string(navigation_probe) +
         ", actual_focus=" + std::to_string(SDL_GetKeyboardFocus() == window.get()) +
         ", navigation_active=" + std::to_string(navigation.active()) +
         ", navigation_context=" + std::to_string(navigation.context()) +
         ", tick=" + std::to_string(observed.simulation_ticks) +
         ", pending=" + std::to_string(navigation.pending()) +
         ", wait_iterations=" + std::to_string(observed.navigation_wait_iterations) +
         ", release_sequence=" + std::to_string(observed.navigation_acks[1].sequence) +
         ", release_delivery_tick=" + std::to_string(observed.navigation_acks[1].delivery_tick) +
         ", release_sampled_tick=" + std::to_string(observed.navigation_acks[1].sampled_tick) +
         ", external_events=" + std::to_string(observed.navigation_external_events);
}
void RuntimeLoop::key(std::size_t index, bool down, bool repeat) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.timestamp = navigation_probe_token + index + 1;
  event.key.windowID = window_id;
  event.key.scancode = SDL_SCANCODE_D;
  event.key.key = SDLK_D;
  event.key.repeat = repeat;
  sdl_check(SDL_PushEvent(&event));
}
void RuntimeLoop::focus(std::size_t index, bool active) {
  SDL_Event event{};
  event.type = active ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
  event.window.timestamp = navigation_probe_token + index + 1;
  event.window.windowID = window_id;
  sdl_check(SDL_PushEvent(&event));
}
bool RuntimeLoop::smoke_navigation() {
  if (!options.smoke.empty() && navigation_probe > 0 && navigation_probe < 7) {
    // A render is not an input-delivery or fixed-update barrier. Preserve the
    // render budget while polling SDL and sampling normal fixed updates.
    for (auto &ack : observed.navigation_acks)
      if (ack.sequence && !ack.sampled_tick && observed.simulation_ticks > ack.delivery_tick)
        ack.sampled_tick = observed.simulation_ticks;
    const auto sampled = [&](std::size_t index) { return observed.navigation_acks[index].sampled_tick != 0; };
    const auto stationary = [&](const char *message) {
      if (camera.target != navigation_after)
        throw std::runtime_error(std::string{message} + ": " + smoke_state());
    };
    bool ready = false;
    if (navigation_probe == 1 && sampled(0) && camera.target != navigation_before) {
      key(1, false);
      navigation_probe = 2;
      ready = true;
    } else if (navigation_probe == 2 && sampled(1)) {
      stationary("navigation continued after release");
      key(2, true);
      focus(3, false);
      navigation_probe = 3;
      ready = true;
    } else if (navigation_probe == 3 && sampled(2) && sampled(3)) {
      stationary("navigation continued after focus loss");
      focus(4, true);
      key(5, true, true);
      navigation_probe = 4;
      ready = true;
    } else if (navigation_probe == 4 && sampled(4) && sampled(5)) {
      stationary("repeat revived focus-lost movement");
      key(6, true);
      navigation_probe = 5;
      ready = true;
    } else if (navigation_probe == 5 && sampled(6) && camera.target != navigation_before) {
      key(7, false);
      navigation_probe = 6;
      ready = true;
    } else if (navigation_probe == 6 && sampled(7)) {
      stationary("navigation continued after fresh-press release");
      camera.orbit(40, -12);
      camera.zoom(.5);
      observed.scripted_camera = true;
      navigation_probe = 7;
      ready = true;
    }
    if (!ready) {
      ++observed.navigation_wait_iterations;
      SDL_Delay(1);
      return false;
    }
  }
  return true;
}
void RuntimeLoop::smoke_frame(bool focused) {
  if (!options.smoke.empty()) {
    if (observed.rendered == 1)
      check(viewport->capture(options.smoke / "initial.ppm"));
    if (observed.rendered >= 2 && !resize_requested) {
      sdl_check(SDL_SetWindowSize(window.get(), 1000, 650));
      resize_requested = true;
    }
    if (observed.rendered >= 4 && !minimize_requested) {
      sdl_check(SDL_MinimizeWindow(window.get()));
      minimize_requested = true;
      minimize_time = Clock::now();
    }
    if (observed.rendered >= 6 && navigation_probe == 0 && observed.restored && focused) {
      key(0, true);
      navigation_probe = 1;
    }
  }
  if (options.frames && observed.rendered >= options.frames && !close_requested) {
    SDL_Event close{};
    close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    close.window.windowID = window_id;
    sdl_check(SDL_PushEvent(&close));
    close_requested = true;
  }
}

void RuntimeLoop::finish_smoke() {
  if (observed.rendered != options.frames || !observed.resized || !observed.minimized || !observed.restored ||
      !observed.close_event || !observed.scripted_camera)
    throw std::runtime_error("smoke lifecycle incomplete: " + smoke_state());
  check(viewport->capture(options.smoke / "runtime.ppm"));
  check(document->stop_play());
  check(document->cook(*services.files, utf8(options.smoke / "runtime-after-play.rscene")));
  observed.edit_unchanged =
      authored_bytes ==
      take(services.files->read(utf8(options.smoke / "runtime-after-play.rscene"), scene_byte_limit));
  observed.elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  const auto stats = viewport->stats();
  save_receipt(options, observed, stats, last_frame);
  if (!observed.edit_unchanged || stats.device_lost || stats.debug_errors ||
      stats.completed_frames != stats.submitted_frames)
    throw std::runtime_error("runtime final Scene/GPU checks failed; see receipt");
  std::cout << "Runtime smoke passed: " << observed.rendered
            << " GPU frames, Scene parity, resize/minimize/restore/close; " << observed.elapsed_ms
            << " ms; receipt=" << utf8(options.smoke / "runtime-receipt.json") << '\n';
}

} // namespace race_runtime
