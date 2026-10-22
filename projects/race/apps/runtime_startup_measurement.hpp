#pragma once
#include "startup_measurement.hpp"
#include "../src/render/windows_viewport.hpp"
#include <SDL3/SDL.h>
#include <memory>

namespace race_runtime {
struct Options;
struct ProfileData;
void size_runtime_viewport(race::desktop::startup::Witness *, SDL_Window *);
void initialize_runtime_profile(const Options &, ProfileData &);
std::unique_ptr<race::desktop::startup::Witness> runtime_measurement(const Options &);
void runtime_measurement_frame(race::desktop::startup::Witness *, race::render::WindowsViewport &, HWND);
} // namespace race_runtime
