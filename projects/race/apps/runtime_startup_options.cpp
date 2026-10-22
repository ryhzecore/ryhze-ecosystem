#include "runtime_session.hpp"

namespace race_runtime {
Options parse(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (argument != "--scene" && argument != "--smoke" && argument != "--profile" && argument != "--frames" &&
        argument != "--shaders" && argument != "--assets" && argument != "--mesh-smoke" &&
        argument != "--package" && argument != "--epoch-smoke" && argument != "--startup-measurement")
      throw std::runtime_error("unknown argument; use --help");
    if (++i == argc || std::string_view(argv[i]).empty())
      throw std::runtime_error("missing option value");
    if (argument == "--frames") {
      const std::string_view text(argv[i]);
      const auto result = std::from_chars(text.data(), text.data() + text.size(), options.frames);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !options.frames ||
          options.frames > 3600)
        throw std::runtime_error("--frames must be an integer from 1 through 3600");
    } else {
      const auto path = fs::absolute(from_utf8(argv[i])).lexically_normal();
      if (argument == "--scene")
        options.scene = path;
      if (argument == "--smoke")
        options.smoke = path;
      if (argument == "--profile")
        options.profile = path;
      if (argument == "--shaders")
        options.shaders = path;
      if (argument == "--assets")
        options.assets = path;
      if (argument == "--package")
        options.package = path;
      if (argument == "--mesh-smoke")
        options.mesh_smoke = path;
      if (argument == "--epoch-smoke")
        options.epoch_smoke = path;
      if (argument == "--startup-measurement")
        options.startup = path;
    }
  }
  if (!options.smoke.empty()) {
    if (!options.frames)
      options.frames = 32;
    if (options.frames < 24)
      throw std::runtime_error("--smoke needs at least 24 frames for lifecycle and input checks");
  }
  if (!options.profile.empty()) {
    if (!options.smoke.empty())
      throw std::runtime_error("--profile and --smoke are separate runs");
    if (!options.frames)
      options.frames = 660;
    if (options.frames <= 60)
      throw std::runtime_error("--profile needs 60 warmup frames followed by measured frames");
  }
  if (!options.mesh_smoke.empty() && (!options.smoke.empty() || !options.profile.empty() ||
                                      (options.scene.empty() && options.package.empty())))
    throw std::runtime_error("--mesh-smoke requires --scene or --package and a separate run");
  if (!options.epoch_smoke.empty()) {
    if (!options.smoke.empty() || !options.profile.empty() || !options.mesh_smoke.empty() ||
        (options.frames && options.frames != 1))
      throw std::runtime_error("--epoch-smoke requires a separate one-frame run");
    options.frames = 1;
  }
  return options;
}

} // namespace race_runtime
