"""Small allowlist guard: explicit CMake dependencies plus portable include edges."""
import pathlib
import re
import sys
root = pathlib.Path(sys.argv[1])
allowed = {
    "race_jolt_abi": {"Jolt"}, "race_jolt_allocation_probe": {"Jolt"}, "Jolt": set(),
    "race_physics_profile": {"race_foundation"},
    "race_physics_profile_tests": {"race_physics_profile"},
    "race_collision_product_tests": {"race_assets", "platform_native"},
    "race_desktop_gizmo": {"race_scene", "race_render"},
    "race_desktop_gizmo_tests": {"race_desktop_gizmo"},
    "race_foundation": {"race_foundation_digest"}, "race_foundation_digest": set(), "race_platform_api": {"race_foundation"},
    "platform_linux": {"race_platform_api", "Threads::Threads"},
    "platform_windows": {"race_platform_api", "Threads::Threads", "Psapi", "Ntdll"},
    "platform_native": {"platform_windows", "platform_linux"},
    "race_process_tests": {"platform_native", "Threads::Threads"},
    "race_directory_invalidation_tests": {"race_platform_api"},
    "race_directory_watch_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_platform_input": {"race_platform_api", "Threads::Threads"},
    "platform_sdl": {"race_platform_input", "SDL3::SDL3"},
    "race_platform_input_tests": {"race_platform_input", "Threads::Threads"},
    "race_sdl_input_tests": {"platform_sdl", "SDL3::SDL3"},
    "race_core": {"race_platform_api", "Threads::Threads"},
    "race_runtime": {"race_core"},
    "race_scene": {"race_core"},
    "race_assets": {"race_core"},
    "race_atmosphere": {"race_foundation"},
    "race_atmosphere_tests": {"race_atmosphere"},
    "race_atmosphere_demo": {"race_atmosphere"},
    "race_audio": {"race_foundation"},
    "race_audio_tests": {"race_audio"},
    "race_audio_demo": {"race_audio"},
    "race_vfx": {"race_foundation"},
    "race_vfx_tests": {"race_vfx"},
    "race_vfx_demo": {"race_vfx"},
    "race_render": {"race_foundation", "Threads::Threads"},
    "race_render_tests": {"race_render"},
    "race_render_resource_contract_tests": {"race_render"},
    "race_render_graph_tests": {"race_render"},
    "race_light_output_tests": {"race_render"},
    "race_mesh_render": {"race_assets", "race_render"},
    "race_render_fixture": {"race_render", "race_scene", "race_assets", "platform_native"},
    "race_surface_pipeline": {"race_assets"},
    "race_surface_tool": {"race_surface_pipeline", "platform_native"},
    "race_surface_pipeline_tests": {"race_surface_pipeline", "platform_native"},
    "race_surface_recipe_tests": {"race_surface_pipeline", "platform_native", "Threads::Threads"},
    "race_project_index": {"race_assets"},
    "race_content_project": {"race_mesh_import", "race_surface_pipeline", "race_project_index"},
    "race_content_mesh_checkpoint": {"race_content_project", "race_mesh_import"},
    "race_surface_checkpoint": {"race_content_project"},
    "race_desktop_surface_reimport": {"race_desktop_project_browser", "race_surface_checkpoint", "Threads::Threads"},
    "race_desktop_surface_checkpoint": {"race_desktop_project_browser", "race_surface_checkpoint", "race_desktop_meshes", "Threads::Threads"},
    "race_content_mesh_checkpoint_tests": {"race_content_mesh_checkpoint", "race_surface_checkpoint", "platform_native", "Threads::Threads"},
    "race_surface_checkpoint_tests": {"race_surface_checkpoint", "platform_native", "Threads::Threads"},
    "race_desktop_surface_reimport_tests": {"race_desktop_surface_reimport", "platform_native", "Threads::Threads"},
    "race_desktop_surface_checkpoint_tests": {"race_desktop_surface_checkpoint", "platform_native", "Threads::Threads"},
    "race_content_tool": {"race_content_project", "platform_native"},
    "race_content_project_tests": {"race_content_project", "platform_native", "Threads::Threads"},
    "race_native_surface_tests": {"race_mesh_pipeline", "platform_native"},
    "race_viewport_material_tests": {"race_render"},
    "race_desktop_material_assignment": {"race_desktop_meshes", "Threads::Threads"},
    "race_desktop_material_assignment_tests": {"race_desktop_material_assignment", "race_surface_pipeline", "platform_native", "Threads::Threads"},
    "race_desktop_material_tests": {"race_desktop_meshes", "race_mesh_pipeline", "platform_native"},
    "race_mesh_pipeline": {"race_assets"},
    "race_mesh_import": {"race_mesh_pipeline", "race_project_index"},
    "race_mesh_import_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_mesh_recipe_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_mesh_project_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_mesh_checkpoint_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_mesh_watch_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_mesh_reimport_tests": {"race_mesh_import", "platform_native", "Threads::Threads"},
    "race_mesh_import_worker": {"race_mesh_pipeline", "platform_native"},
    "race_mesh_import_tool": {"race_mesh_import", "platform_native"},
    "race_mesh_tool": {"race_mesh_pipeline", "platform_native"},
    "race_assets_tests": {"race_assets", "race_runtime", "race_mesh_pipeline", "race_mesh_render", "platform_native"},
    "race_assets_fuzz": {"race_assets", "platform_native"},
    "race_assets_tool": {"race_assets", "platform_native"},
    "race_assets_headless": {"race_assets", "race_scene", "race_runtime", "platform_native"},
    "race_scene_headless": {"race_scene", "race_runtime", "platform_native"},
    "race_scene_workloads": {"race_scene", "platform_native"},
    "race_effects_headless": {"race_scene", "race_runtime", "race_audio", "race_vfx", "platform_native"},
    "race_scene_tests": {"race_scene", "platform_native"},
    "race_headless": {"race_runtime", "platform_native"},
    "race_tests": {"race_runtime", "platform_native"},
    "race_benchmarks": {"race_runtime", "platform_native"},
    "race_scheduler_workloads": {"race_core", "platform_native"},
    "race_scene_identity_tests": {"race_foundation", "Threads::Threads"},
    "race_desktop_scene": {"race_scene"},
    "race_desktop_meshes": {"race_desktop_scene", "race_mesh_render"},
    "race_desktop_project_browser": {"race_content_project", "Threads::Threads"},
    "race_desktop_project_browser_tests": {"race_desktop_project_browser", "platform_native", "Threads::Threads"},
    "race_desktop_project_authoring": {"race_desktop_project_browser", "race_mesh_import", "Threads::Threads"},
    "race_desktop_project_authoring_tests": {"race_desktop_project_authoring", "platform_native", "Threads::Threads"},
    "race_desktop_mesh_reimport": {"race_content_mesh_checkpoint", "race_desktop_project_browser", "race_mesh_import", "Threads::Threads"},
    "race_editor_reimport_worker_tests": {"race_desktop_mesh_reimport", "platform_native"},
    "race_desktop_mesh_reimport_tests": {"race_desktop_mesh_reimport", "platform_native", "Threads::Threads"},
    "race_desktop_mesh_checkpoint": {"race_content_mesh_checkpoint", "race_desktop_meshes", "race_mesh_import", "Threads::Threads"},
    "race_desktop_mesh_checkpoint_tests": {"race_desktop_mesh_checkpoint", "platform_native", "Threads::Threads"},
    "race_package_tool": {"race_desktop_meshes", "platform_native"},
    "race_desktop_package_tests": {"race_desktop_meshes", "race_mesh_pipeline", "platform_native"},
    "race_desktop_meshes_tests": {"race_desktop_meshes", "race_mesh_pipeline", "platform_native"},
    "race_desktop_scene_tests": {"race_desktop_scene", "platform_native"},
    "race_desktop_persistence_tests": {"race_desktop_scene", "Threads::Threads"},
    "race_desktop_navigation_tests": {"race_foundation"},
    "race_desktop_profile": set(),
    "race_desktop_profile_tests": {"Threads::Threads"},
    "render_windows": {"race_render", "d3d12", "dxgi", "dxguid"},
    "render_windows_removal_probe": {"race_render", "d3d12", "dxgi", "dxguid"},
    "race_viewport_removal_tests": {"render_windows_removal_probe", "user32"},
    "race_viewport_tests": {"render_windows", "race_mesh_render", "race_mesh_pipeline", "platform_native", "user32"},
    "race_editor": {"race_desktop_surface_reimport", "race_desktop_surface_checkpoint", "race_desktop_material_assignment", "race_desktop_project_authoring", "race_desktop_mesh_reimport", "race_desktop_project_browser", "race_desktop_gizmo", "race_desktop_mesh_checkpoint", "race_desktop_scene", "race_desktop_meshes", "race_desktop_profile", "render_windows", "platform_native", "Qt6::Core", "Qt6::Gui", "Qt6::Widgets"},
    "race_runtime_desktop": {"race_desktop_scene", "race_desktop_meshes", "race_desktop_profile", "render_windows", "platform_native", "platform_sdl", "SDL3::SDL3"},
}
cmake = (root / "CMakeLists.txt").read_text()
cmake += (root / "tests/physics_qualification/CMakeLists.txt").read_text()
edges = {key: set() for key in allowed}
for body in re.findall(r"target_link_libraries\(([^)]+)\)", cmake):
    target, *deps = body.split()
    assert target in allowed, f"unknown target {target} requires layer review"
    deps = set(deps) - {"PUBLIC", "PRIVATE", "INTERFACE"}
    assert deps <= allowed[target], f"disallowed dependencies {target}: {deps}"
    edges[target] |= deps
def visit(node, stack):
    assert node not in stack, f"dependency cycle: {stack} -> {node}"
    for child in edges.get(node, ()):
        visit(child, stack + [node])
for node in edges: visit(node, [])
layers = {"foundation": 0, "platform": 1, "core": 2, "runtime": 3, "scene": 3, "assets": 3, "render": 3, "audio": 3, "vfx": 3, "atmosphere": 3, "physics": 3}
for folder in (root / "include/race", root / "src"):
    for path in folder.rglob("*"):
        if path.suffix not in {".cpp", ".hpp"}: continue
        relative = path.relative_to(folder)
        layer = relative.parts[0]
        if layer == "platform" and folder.name == "src": continue
        # Narrow private Render adapter exception; portable render headers remain checked.
        if folder.name == "src" and relative.as_posix() in {
            "render/windows_viewport.hpp",
            "render/windows_viewport_internal.hpp",
            "render/windows_viewport_device.cpp",
            "render/windows_viewport_resources.cpp",
            "render/windows_viewport_graph.cpp",
            "render/windows_viewport_prepare.cpp",
            "render/windows_viewport_draw.cpp",
            "render/windows_viewport_capture.cpp",
            "render/windows_viewport.cpp",
            "render/windows_viewport_pipeline.cpp",
        }: continue
        text = path.read_text()
        for header in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)', text, re.M):
            assert not re.search(r"^(sys/|linux/|windows|unistd|pthread|vulkan|GL/|Metal/|d3d|SDL|Jolt/)", header, re.I), f"native include {path}: {header}"
            if header.startswith("race/"):
                target = header.split('/')[1]
                assert target in layers and layers[target] <= layers[layer], f"upward include {path}: {header}"
                assert not (layer != target and layers[layer] == 3 and layers[target] == 3), f"sibling include {path}: {header}"
                assert not (layer == "foundation" and target != layer)
        assert not re.search(r"\b(HWND|VkDevice|ID3D12Device|pthread_t|SDL_Event|SDL_Window|SDL_Scancode)\b", text), f"native public type {path}"
        assert not re.search(r"\bJPH\s*::", text), f"SDK type outside private adapter {path}"
print("architecture: acyclic allowlisted targets and portable includes passed")
