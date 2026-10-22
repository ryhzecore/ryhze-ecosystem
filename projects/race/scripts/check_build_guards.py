"""Exercise guard rejection without modifying the real source checkout."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
from source_manifest import ROOT, authored_inputs
with tempfile.TemporaryDirectory(prefix="race-build-guards-") as name:
    root = pathlib.Path(name)
    for folder in ("include", "src", "scripts", "tests", "apps", "benchmarks", "shaders"):
        shutil.copytree(ROOT / folder, root / folder, ignore=shutil.ignore_patterns("__pycache__"))
    for filename in ("CMakeLists.txt", "CMakePresets.json"):
        shutil.copy2(ROOT / filename, root / filename)
    def check(expected):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check_architecture.py"), str(root)], capture_output=True, timeout=10)
        assert (result.returncode == 0) == expected, result.stderr.decode()
    check(True)
    initial = authored_inputs(root)["sha256"]
    (root / "notes.md").write_text("documentation only")
    assert authored_inputs(root)["sha256"] == initial
    path = root / "CMakeLists.txt"; original = path.read_text()
    path.write_text(original + "\ntarget_link_libraries(race_core PRIVATE race_runtime)\n")
    assert authored_inputs(root)["sha256"] != initial
    check(False)
    path.write_text(original)
    header = root / "include/race/foundation/result.hpp"
    original_header = header.read_text()
    header.write_text(original_header + "\n#include <windows.h>\n")
    check(False)
    header.write_text(original_header + '\n#include "race/core/jobs.hpp"\n')
    check(False)
    header.write_text(original_header + "\n#include <SDL3/SDL.h>\n")
    check(False)
    header.write_text(original_header)
    asset_header = root / "include/race/assets/assets.hpp"
    original_asset = asset_header.read_text()
    asset_header.write_text(original_asset + '\n#include "race/scene/scene.hpp"\n')
    check(False)
    asset_header.write_text(original_asset)
    render_header=root / "include/race/render/fixture.hpp"
    original_render=render_header.read_text()
    for forbidden in ['race/scene/scene.hpp','race/assets/assets.hpp','vulkan/vulkan.h']:
        render_header.write_text(original_render+'\n#include "'+forbidden+'"\n')
        check(False)
    render_header.write_text(original_render)
    path.write_text(original + "\ntarget_link_libraries(race_assets PRIVATE race_runtime)\n")
    check(False)
print("guard rejection: upward target, native header, upward include and changed authored input passed")
