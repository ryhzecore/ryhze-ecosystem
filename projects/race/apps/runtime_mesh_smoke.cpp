#include "runtime_session.hpp"

namespace race_runtime {
void mesh_receipt_identity(std::ostream &out, const Options &options, const ProfileData &profile,
                           const std::string &package_scene_id, const std::string &package_scene_revision,
                           const race::render::ViewportStats &s, const race::render::RenderFrame &frame,
                           bool passed, bool parity) {
  out << "{\"schema\":1,\"application\":\"SDL\",\"passed\":" << passed
      << ",\"authoring_preserved\":" << parity << ",\"scene_source\":"
      << json_string(!options.package.empty() ? utf8(options.package) : utf8(options.scene))
      << ",\"scene_source_kind\":" << json_string(options.package.empty() ? "file" : "package")
      << ",\"scene_load_ms\":" << profile.scene_load_ms
      << ",\"package_scene_id\":" << json_string(package_scene_id)
      << ",\"package_scene_revision\":" << json_string(package_scene_revision)
      << ",\"adapter\":" << json_string(s.adapter) << ",\"hardware\":" << s.hardware
      << ",\"debug_layer\":" << s.debug_layer << ",\"debug_errors\":" << s.debug_errors
      << ",\"debug_warnings\":" << s.debug_warnings << ",\"width\":" << s.width << ",\"height\":" << s.height
      << ",\"source_commit\":" << json_string(race::build::source_commit)
      << ",\"authored_input_sha256\":" << json_string(race::build::authored_input_sha256)
      << ",\"lighting_profile\":" << unsigned(frame.lighting_profile)
      << ",\"physical_output_installed\":" << s.physical_output_installed
      << ",\"exposure_scale\":" << s.exposure_scale << ",\"meshes\":[";
}
void mesh_receipt_assets(std::ostream &out, const std::shared_ptr<const race::desktop::MeshSet> &meshes) {
  bool comma = false;
  for (const auto &e : meshes->entries()) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"asset\":" << json_string(race::assets::id_text(e.reference.asset))
        << ",\"revision\":" << json_string(race::assets::hex(e.reference.revision))
        << ",\"vertices\":" << e.geometry->vertices().size()
        << ",\"indices\":" << e.geometry->indices().size() << '}';
  }
  out << "],\"materials\":[";
  comma = false;
  for (const auto &e : meshes->materials()) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"asset\":" << json_string(race::assets::id_text(e.reference.asset))
        << ",\"revision\":" << json_string(race::assets::hex(e.reference.revision)) << ",\"sampling\":"
        << json_string(e.material->sampling() == race::render::TextureSampling::LinearClampMips
                           ? "linear-clamp-mips"
                           : "linear-clamp-mip0")
        << ",\"texture_role\":"
        << json_string(e.material->texture_role() == race::render::TextureRole::DiffuseReflectanceR
                           ? "diffuse-reflectance-r"
                           : "color")
        << '}';
  }
  out << "],\"textures\":[";
  comma = false;
  for (const auto &e : meshes->textures()) {
    if (comma)
      out << ',';
    comma = true;
    out << "{\"asset\":" << json_string(race::assets::id_text(e.reference.asset))
        << ",\"revision\":" << json_string(race::assets::hex(e.reference.revision))
        << ",\"width\":" << e.texture->width() << ",\"height\":" << e.texture->height()
        << ",\"mip_count\":" << e.texture->mip_count() << ",\"texel_bytes\":" << e.texture->texels().size()
        << ",\"encoding\":"
        << json_string(e.texture->encoding() == race::render::TextureEncoding::LinearData ? "linear-data"
                                                                                          : "srgb-color")
        << '}';
  }
}
int RuntimeSession::mesh_smoke(const std::vector<race::desktop::MeshReference> &mesh_refs,
                               std::shared_ptr<const race::desktop::MeshSet> &meshes,
                               const std::string &package_scene_id, const std::string &package_scene_revision,
                               std::unique_ptr<race::render::WindowsViewport> &viewport) {
  if (fs::exists(options.mesh_smoke))
    throw std::runtime_error("mesh smoke needs a fresh directory");
  fs::create_directories(options.mesh_smoke);
  if (mesh_refs.empty())
    throw std::runtime_error("mesh smoke requires at least one pinned mesh");
  SDL_PumpEvents();
  check(viewport->resize(640, 480));
  auto frame = take(race::desktop::scene_frame(*document, meshes.get(), {}));
  check(viewport->draw(frame));
  check(viewport->capture(options.mesh_smoke / "scene.ppm"));
  for (auto &draw : frame.draws)
    draw.geometry.reset();
  check(viewport->draw(frame));
  check(viewport->capture(options.mesh_smoke / "cube-control.ppm"));
  check(document->save(*services.files, utf8(options.mesh_smoke / "mesh.scene.json")));
  check(document->cook(*services.files, utf8(options.mesh_smoke / "mesh.rscene")));
  check(document->start_play());
  check(document->tick_play(fixed_step));
  auto play = take(race::desktop::scene_frame(*document, meshes.get(), {}, true, 1));
  check(viewport->draw(play));
  check(viewport->capture(options.mesh_smoke / "play.ppm"));
  check(document->stop_play());
  check(document->cook(*services.files, utf8(options.mesh_smoke / "after-play.rscene")));
  const bool parity =
      take(services.files->read(utf8(options.mesh_smoke / "mesh.rscene"), scene_byte_limit)) ==
      take(services.files->read(utf8(options.mesh_smoke / "after-play.rscene"), scene_byte_limit));
  auto s = viewport->stats();
  const bool passed = parity && s.hardware && !s.device_lost && !s.debug_errors && !s.debug_warnings &&
                      s.completed_frames == s.submitted_frames && s.width == 640 && s.height == 480;
  std::ofstream out(options.mesh_smoke / "mesh-receipt.json", std::ios::binary);
  out << std::boolalpha;
  mesh_receipt_identity(out, options, profile, package_scene_id, package_scene_revision, s, frame, passed,
                        parity);
  mesh_receipt_assets(out, meshes);
  out << "],\"texture_uploads\":" << s.texture_uploads
      << ",\"texture_upload_bytes\":" << s.texture_upload_bytes
      << ",\"texture_cache_hits\":" << s.texture_cache_hits << "}\n";
  out.flush();
  if (!out || !passed)
    throw std::runtime_error("mesh smoke GPU/Scene checks failed");
  return 0;
}

} // namespace race_runtime
