#include "runtime_session.hpp"
#ifdef _MSC_VER
#pragma warning(push)
// SDL's Windows bootstrap redeclares WinMain without the Windows SDK's SAL
// annotations. Suppress that include-local mismatch, not application analysis.
#pragma warning(disable : 28251)
#endif
#include <SDL3/SDL_main.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

using race_runtime::parse;
using race_runtime::run;

int main(int argc, char **argv) {
  try {
    const auto options = parse(argc, argv);
    if (options.help) {
      std::cout
          << "RACE SDL3/D3D12 Runtime\n"
             "  --scene PATH   Load shared editor JSON or cooked RACESCN1 .rscene (default box otherwise)\n"
             "  --assets DIR   Root containing cooked, revision-named .rasset mesh products\n"
             "  --package FILE Cooked Scene and meshes in one read-only .rpak\n"
             "  --mesh-smoke DIR  Fixed-camera mesh/Play GPU captures; requires --scene or --package\n"
             "  --epoch-smoke DIR  Separate controlled viewport recreation and authority checks\n"
             "  --shaders DIR  DXIL directory (default: executable sibling shaders/)\n"
             "  --frames N     Exit through the event loop after 1..3600 rendered frames\n"
             "  --smoke DIR    Fresh output directory; defaults to 32 frames, requires at least 24\n"
             "  --profile DIR  Separate fixed1280x720 run; default660 frames (60 warmup + 600 measured)\n"
             "Controls: right-drag orbit, wheel zoom, WASD move, Q/E vertical, Escape/close exits.\n"
             "Smoke writes real GPU PPM readbacks, binary Scene roundtrip files and a JSON receipt.\n";
      return 0;
    }
    return run(options);
  } catch (const std::exception &error) {
    std::cerr << "RACE Runtime failed: " << error.what() << '\n';
    return 1;
  }
}
