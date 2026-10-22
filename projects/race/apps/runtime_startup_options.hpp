#pragma once
#include <cstdint>
#include <filesystem>

namespace race_runtime {
struct Options {
  std::filesystem::path scene, smoke, profile, shaders, assets, mesh_smoke, package, epoch_smoke, startup;
  std::uint64_t frames{};
  bool help{};
};
Options parse(int argc, char **argv);
} // namespace race_runtime
