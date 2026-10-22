#pragma once
#include "race/platform/input.hpp"
#include <SDL3/SDL.h>

namespace race::platform::input {
// Private executable-composition factory. SDL and this window must outlive the
// returned service; creation/poll/close run on SDL's main thread. No event watch
// is registered, so OS background/suspend callbacks are outside this profile.
// Exclusive single-window ownership of the process SDL event queue is required:
// foreign-window records are consumed as ignored; another poller cannot coexist.
// SDL polling can stall during native drag/resize. An empty poll can be the SDL
// poll sentinel boundary, not proof of global queue quiescence.
inline constexpr std::uint64_t sdl_event_clock_domain=0x53444c334e53;
Result<std::unique_ptr<Events>> make_sdl_events(SDL_Window*,Clock&,
    std::uint64_t service,const Limits& = {});
}
