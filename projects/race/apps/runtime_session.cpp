#include "runtime_session.hpp"

namespace race_runtime {
int run(const Options &options) {
  const auto application_start = Clock::now();
  const bool profiling = !options.profile.empty();
  ProfileData profile;
  initialize_runtime_profile(options, profile);
  auto startup = runtime_measurement(options);
  race::desktop::profile::AllocationScope allocation_scope(profiling);
  profile.allocation_active = allocation_scope.active();
  if (profiling && profile.allocation_available != profile.allocation_active)
    throw std::runtime_error("profile allocation counter could not enter its declared scope");
  auto services = take(race::platform::make_services());
  const auto create_start = Clock::now();
  auto document = take(race::desktop::Document::create());
  profile.document_create_ms = milliseconds(create_start, Clock::now());
  RuntimeSession session{options, application_start, profiling, profile,
                         startup, allocation_scope,  services,  document};
  return session.scene();
}
int RuntimeSession::scene() {
  const auto load_start = Clock::now();
  if (!options.package.empty() && (!options.scene.empty() || !options.assets.empty()))
    throw std::runtime_error("--package is exclusive with --scene and --assets");
  std::shared_ptr<const race::desktop::MeshSet> package_meshes;
  std::string package_scene_id, package_scene_revision;
  if (!options.package.empty()) {
    auto prepared =
        take(race::desktop::read_package(*services.files, utf8(options.package), *document->schemas()));
    check(document->install(prepared.scene, document->persistence_stamp()));
    package_meshes = std::move(prepared.meshes);
    package_scene_id = race::assets::id_text(prepared.scene_key.id);
    package_scene_revision = race::assets::hex(prepared.scene_key.revision);
  }

  std::shared_ptr<race::platform::RootedFiles> asset_files;
  if (!options.assets.empty())
    asset_files = take(race::platform::make_rooted_files(utf8(options.assets)));
  const auto prepared_snapshot =
      options.scene.empty()
          ? take(document->snapshot())
          : take(race::desktop::Document::decode(*services.files, utf8(options.scene), *document->schemas()));
  const auto mesh_refs = take(race::desktop::mesh_references(prepared_snapshot));
  const auto material_refs = take(race::desktop::material_references(prepared_snapshot));
  auto meshes =
      package_meshes
          ? std::move(package_meshes)
          : take(race::desktop::MeshSet::prepare(std::move(asset_files), mesh_refs, {}, material_refs));
  check(meshes->validate_scene(prepared_snapshot));
  if (!options.scene.empty())
    check(document->install(prepared_snapshot, document->persistence_stamp()));
  if (startup)
    startup->adopted(take(document->objects()).size());
  profile.scene_load_ms =
      options.scene.empty() && options.package.empty() ? 0 : milliseconds(load_start, Clock::now());
  profile.package_scene_id = package_scene_id;
  profile.package_scene_revision = package_scene_revision;
  if (!options.epoch_smoke.empty()) {
    if (fs::exists(options.epoch_smoke) || !fs::create_directory(options.epoch_smoke))
      throw std::runtime_error("epoch smoke requires a fresh directory with an existing parent");
  }
  if (profiling)
    check(document->cook(*services.files, utf8(options.profile / "profile-scene.rscene")));
  return window(mesh_refs, meshes, package_scene_id, package_scene_revision);
}

int RuntimeSession::window(const std::vector<race::desktop::MeshReference> &mesh_refs,
                           std::shared_ptr<const race::desktop::MeshSet> &meshes,
                           const std::string &package_scene_id, const std::string &package_scene_revision) {
  Observations observed;
  observed.scene_load_ms = profile.scene_load_ms;
  observed.package_scene_id = package_scene_id;
  observed.package_scene_revision = package_scene_revision;
  std::vector<std::byte> authored_bytes;
  if (!options.smoke.empty()) {
    fs::create_directories(options.smoke);
    for (const auto *name : {"runtime-receipt.json", "runtime-saved.rscene", "runtime-roundtrip.rscene",
                             "runtime-after-play.rscene", "initial.ppm", "runtime.ppm"})
      if (fs::exists(options.smoke / name))
        throw std::runtime_error("smoke output already exists; choose a fresh directory");
    check(document->cook(*services.files, utf8(options.smoke / "runtime-saved.rscene")));
    authored_bytes =
        take(services.files->read(utf8(options.smoke / "runtime-saved.rscene"), scene_byte_limit));
    auto reloaded = take(race::desktop::Document::create());
    check(reloaded->load(*services.files, utf8(options.smoke / "runtime-saved.rscene")));
    check(reloaded->cook(*services.files, utf8(options.smoke / "runtime-roundtrip.rscene")));
    observed.parity =
        authored_bytes ==
        take(services.files->read(utf8(options.smoke / "runtime-roundtrip.rscene"), scene_byte_limit));
    observed.scene_bytes = authored_bytes.size();
    if (!observed.parity)
      throw std::runtime_error("canonical Scene binary roundtrip differs");
  }
  if (options.mesh_smoke.empty())
    check(document->start_play());
  if (!options.epoch_smoke.empty()) {
    check(document->tick_play(fixed_step));
    observed.simulation_ticks = 1;
  }
  SdlLifetime sdl;
  std::unique_ptr<SDL_Window, WindowDelete> window(
      SDL_CreateWindow("RACE Runtime | Right-drag orbit | Wheel zoom | WASD move | Q/E vertical", 1280, 720,
                       SDL_WINDOW_HIGH_PIXEL_DENSITY | (profiling || startup ? 0 : SDL_WINDOW_RESIZABLE)));
  if (!window)
    throw std::runtime_error(SDL_GetError());
  size_runtime_viewport(startup.get(), window.get());
  auto *hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window.get()),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!hwnd)
    throw std::runtime_error("SDL did not provide the native Windows window");
  fs::path shader_directory = options.shaders;
  if (shader_directory.empty()) {
    const auto *base = SDL_GetBasePath();
    if (!base)
      throw std::runtime_error(SDL_GetError());
    shader_directory = from_utf8(base) / "shaders";
  }
  auto viewport = take(race::render::WindowsViewport::create(hwnd, shader_directory));
  if (!options.mesh_smoke.empty())
    return mesh_smoke(mesh_refs, meshes, package_scene_id, package_scene_revision, viewport);
  return this->viewport(meshes, observed, authored_bytes, window, hwnd, viewport);
}
int RuntimeSession::viewport(std::shared_ptr<const race::desktop::MeshSet> &meshes, Observations &observed,
                             std::vector<std::byte> &authored_bytes,
                             std::unique_ptr<SDL_Window, WindowDelete> &window, HWND hwnd,
                             std::unique_ptr<race::render::WindowsViewport> &viewport) {
  // Owned on the heap: run() already holds several ViewportStats copies (each
  // carries two 512-byte diagnostic buffers), and the epoch-smoke before/after
  // pair pushed the frame past the 16 KiB static-analysis stack threshold (C6262).
  const auto initial_stats_owner = std::make_unique<race::render::ViewportStats>(viewport->stats());
  const auto &initial_stats = *initial_stats_owner;
  if (profiling && (initial_stats.width != 1280 || initial_stats.height != 720))
    throw std::runtime_error("profile requires an actual1280x720 drawable");
  observed.initial_width = initial_stats.width;
  observed.initial_height = initial_stats.height;
  const auto initial_rows = take(document->objects(true));
  const auto expected_draws = static_cast<std::size_t>(
      std::count_if(initial_rows.begin(), initial_rows.end(), [](const auto &row) { return row.visible; }));
  std::cout << "RACE Runtime: SDL " << SDL_GetVersion() << ", D3D12 " << initial_stats.adapter
            << ", hardware=" << std::boolalpha << initial_stats.hardware
            << ", objects=" << initial_rows.size() << ", scene="
            << (!options.package.empty() ? utf8(options.package)
                : options.scene.empty()  ? "shared defaults"
                                         : utf8(options.scene))
            << '\n'
            << std::flush;

  return loop(meshes, observed, authored_bytes, window, hwnd, viewport, expected_draws);
}

} // namespace race_runtime
