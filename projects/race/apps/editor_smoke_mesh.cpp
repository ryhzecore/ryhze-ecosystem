// Opt-in native mesh smoke: package startup loads, native mesh and material authoring, JSON and cooked
// persistence, dependency and budget rejections, Play isolation and the mesh receipt.
#include "editor_window.hpp"

namespace race_editor {
// The mesh smoke, one stage per frame after any running persistence. With a package startup scene, package
// stages 0-3 run first; stage 3 continues into mesh stage 0 in the same frame.
void Editor::mesh_smoke_step() {
  if (frames_ < 3 || !viewport_->renderer() || persistence_->busy())
    return;
  if (initial_scene_source_ == "package" && package_smoke_stage_ < 4) {
    if (package_smoke_stage_ == 0) {
      mesh_smoke_package_open();
      return;
    }
    if (package_smoke_stage_ == 1) {
      mesh_smoke_package_corrupt();
      return;
    }
    if (package_smoke_stage_ == 2) {
      mesh_smoke_package_missing();
      return;
    }
    mesh_smoke_package_end();
  }
  if (mesh_stage_ == 0) {
    mesh_smoke_setup();
    return;
  }
  if (mesh_stage_ == 1) {
    mesh_smoke_layout();
    return;
  }
  if (mesh_stage_ == 2) {
    mesh_stage_ = 3;
    smoke_check("async_json_reload_admitted",
                start_persistence(desktop::PersistenceKind::Load, mesh_smoke_path("mesh.scene.json")));
    return;
  }
  if (mesh_stage_ == 3) {
    auto snapshot = document_->snapshot();
    auto refs = snapshot ? desktop::mesh_references(snapshot.value())
                         : Result<std::vector<desktop::MeshReference>>{snapshot.error()};
    smoke_check("async_json_full_references", refs && refs.value() == mesh_expected_);
    mesh_stage_ = 4;
    smoke_check("async_cooked_reload_admitted",
                start_persistence(desktop::PersistenceKind::Load, mesh_smoke_path("mesh.rscene")));
    return;
  }
  if (mesh_stage_ == 4) {
    mesh_smoke_invalid_scenes();
    return;
  }
  if (mesh_stage_ != 5)
    return;
  mesh_smoke_captures();
}
// A file in the smoke output folder.
QString Editor::mesh_smoke_path(const char *leaf) const { return smoke_ + "/" + leaf; }
// Package stage 0: framing keeps the loaded camera, a timed synchronous read blocks callback dispatch,
// then the interactive package worker is admitted.
void Editor::mesh_smoke_package_open() {
  const auto loaded_pose = viewport_->camera_pose();
  viewport_->frame_all();
  smoke_check("startup_package_frames_loaded_scene", loaded_pose == viewport_->camera_pose());
  const auto begin = Clock::now();
  const auto callbacks = package_heartbeats_;
  auto synchronous =
      desktop::read_package(*services_.files, path_string(initial_scene_path_), *document_->schemas());
  package_sync_ms_ = milliseconds(Clock::now() - begin);
  smoke_check("synchronous_package_valid_and_blocks_callback_dispatch",
              synchronous && callbacks == package_heartbeats_);
  package_started_ = Clock::now();
  package_pending_heartbeats_ = 0;
  package_max_callback_gap_ms_ = 0;
  package_smoke_stage_ = 1;
  package_previous_callback_ = Clock::now();
  smoke_check("interactive_package_worker_admitted", open_package(initial_scene_path_));
  package_admission_ms_ = milliseconds(Clock::now() - package_started_);
}
// Package stage 1: the interactive load framed the scene and installed its dependencies; an edit records
// history, then a corrupted package copy is admitted.
void Editor::mesh_smoke_package_corrupt() {
  package_async_ms_ = milliseconds(Clock::now() - package_started_);
  package_previous_callback_ = {};
  // Tiny loads can finish before a heartbeat. Record samples without a
  // scheduling threshold; the large-workload harness checks responsiveness.
  const auto loaded_pose = viewport_->camera_pose();
  viewport_->frame_all();
  smoke_check("interactive_package_frames_loaded_scene", loaded_pose == viewport_->camera_pose());
  auto snapshot = document_->snapshot();
  auto refs = snapshot ? desktop::mesh_references(snapshot.value())
                       : Result<std::vector<desktop::MeshReference>>{snapshot.error()};
  smoke_check("interactive_package_dependencies_installed",
              refs && meshes_->validate_instances(refs.value()));
  auto row = rows_.front();
  row.color.x = row.color.x == .1 ? .2 : .1;
  smoke_check("package_history_edit", check(document_->edit(row, document_->generation())));
  refresh();
  package_smoke_meshes_ = meshes_;
  smoke_generation_ = document_->generation();
  smoke_selection_ = selection_;
  smoke_dirty_ = dirty_;
  smoke_authoring_path_ = current_path_;
  auto bytes = services_.files->read(path_string(initial_scene_path_), assets::pak::Limits{}.package_bytes);
  smoke_check("package_corruption_fixture_read", bool(bytes));
  if (bytes) {
    bytes.value().back() ^= std::byte{1};
    smoke_check("package_corruption_fixture_write",
                check(services_.files->replace(path_string(mesh_smoke_path("corrupt.rpak")), bytes.value(),
                                               platform::Durability::AtomicVisibility)));
  }
  smoke_expected_persistence_error_ = ErrorCode::InvalidInput;
  package_smoke_stage_ = 2;
  smoke_check("corrupt_package_worker_admitted", open_package(mesh_smoke_path("corrupt.rpak")));
}
// Package stage 2: the corrupt package preserved the scene, history and packets; a missing package is
// admitted.
void Editor::mesh_smoke_package_missing() {
  smoke_check("corrupt_package_preserves_scene_history_packets",
              !smoke_expected_persistence_error_ && document_->generation() == smoke_generation_ &&
                  meshes_ == package_smoke_meshes_ && selection_ == smoke_selection_ &&
                  dirty_ == smoke_dirty_ && current_path_ == smoke_authoring_path_);
  smoke_expected_persistence_error_ = ErrorCode::NotFound;
  package_smoke_stage_ = 3;
  smoke_check("missing_package_worker_admitted", open_package(mesh_smoke_path("missing.rpak")));
}
// Package stage 3: the missing package preserved the scene and packets, and the history edit is undone.
void Editor::mesh_smoke_package_end() {
  smoke_check("missing_package_preserves_scene_and_packets",
              !smoke_expected_persistence_error_ && document_->generation() == smoke_generation_ &&
                  meshes_ == package_smoke_meshes_);
  smoke_check("failed_package_preserves_undo_redo",
              bool(document_->undo()) && bool(document_->redo()) && bool(document_->undo()));
  refresh();
  package_smoke_meshes_.reset();
  package_smoke_stage_ = 4;
  persistence_smoke_stage_ = 0;
}
// Stage 0, first part: the native mesh is added with undo/redo, then its material is assigned (the worker
// spans frames), undone, redone, cleared and restored; then the second part in the same frame.
void Editor::mesh_smoke_setup() {
  if (material_stage_ == -1)
    return;
  if (!mesh_product_.isEmpty() && material_stage_ == 0) {
    const auto before = rows_.size();
    smoke_check("native_mesh_added",
                add_native_mesh(assets_path_ + "/" + mesh_product_) && rows_.size() == before + 1);
    smoke_check("mesh_undo", bool(document_->undo()) && document_->objects().value().size() == before);
    smoke_check("mesh_redo", bool(document_->redo()) && document_->objects().value().size() == before + 1);
    refresh();
  }
  if (!material_product_.isEmpty() && material_stage_ == 0) {
    for (const auto &row : rows_)
      if (row.mesh) {
        selection_ = row.id;
        break;
      }
    refresh();
    auto *button = findChild<QToolButton *>("UiAssignMaterial");
    if (button) {
      auto *scroll = properties_scroll();
      if (scroll)
        scroll->ensureWidgetVisible(button);
    }
    smoke_check("material_button_visible", button && button->isEnabled() && button->isVisible());
    material_stage_ = -1;
    if (button)
      button->click();
    smoke_check("material_worker_admitted", material_assignment_.busy());
    material_stage_ = 1;
    return;
  }
  if (!material_product_.isEmpty() && material_stage_ == 1) {
    if (material_assignment_.busy())
      return;
    smoke_check("material_assigned",
                selected_row() && selected_row()->material.has_value() && !meshes_->materials().empty());
    const auto ref = selected_row() ? selected_row()->material : std::optional<desktop::MaterialReference>{};
    smoke_check("material_undo", bool(document_->undo()) && !document_->objects().value().back().material);
    refresh();
    smoke_check("material_redo", bool(document_->redo()));
    refresh();
    smoke_check("material_redo_pin", selected_row() && selected_row()->material == ref);
    clear_material_action_->trigger();
    smoke_check("material_clear", selected_row() && !selected_row()->material);
    smoke_check("material_clear_undo", bool(document_->undo()));
    refresh();
    smoke_check("material_restored_pin", selected_row() && selected_row()->material == ref);
    material_stage_ = 2;
  }
  mesh_smoke_inspect();
}
// Stage 0, second part: the pinned references, the framed capture, the read-only inspector reference,
// bounds-pick drags and the root and product rejections, then the async save admission.
void Editor::mesh_smoke_inspect() {
  auto snapshot = document_->snapshot();
  auto refs = snapshot ? desktop::mesh_references(snapshot.value())
                       : Result<std::vector<desktop::MeshReference>>{snapshot.error()};
  smoke_check("pinned_meshes_loaded", refs && !refs.value().empty());
  if (refs)
    mesh_expected_ = refs.value();
  for (const auto &row : rows_)
    if (row.mesh) {
      selection_ = row.id;
      break;
    }
  refresh();
  viewport_->frame_selected();
  const auto selected = viewport_->selected;
  viewport_->selected = {};
  smoke_check("framed_mesh_draw", viewport_->render());
  smoke_check("framed_mesh_capture",
              check(viewport_->renderer()->capture(native_path(mesh_smoke_path("framed.ppm")))));
  viewport_->selected = selected;
  smoke_check("mesh_inspector_reference",
              selected_row() && selected_row()->mesh && !fields_[3][0]->isEnabled() &&
                  asset_id_->isReadOnly() && asset_revision_->isReadOnly() &&
                  asset_id_->text() == QString::fromStdString(assets::id_text(selected_row()->mesh->asset)) &&
                  asset_revision_->text() ==
                      QString::fromStdString(assets::hex(selected_row()->mesh->revision)));
  asset_revision_->selectAll();
  smoke_check("full_revision_selectable", asset_revision_->selectedText().size() == 64 &&
                                              asset_revision_->selectedText() == asset_revision_->text());
  asset_revision_->deselect();
  asset_revision_->setCursorPosition(0);
  smoke_check("bounds_pick_drag_cancel", viewport_->smoke_drag(true));
  smoke_check("bounds_pick_drag_commit_undo", viewport_->smoke_drag(false));
  refresh();
  const auto generation = document_->generation();
  const auto old = meshes_;
  smoke_rejection("wrong_assets_root_rejected", [&] { choose_assets(smoke_); });
  smoke_check("root_failure_preserves_scene_and_packets",
              document_->generation() == generation && meshes_ == old);
  smoke_rejection("missing_product_rejected", [&] { add_native_mesh(assets_path_ + "/missing.rasset"); });
  smoke_check("add_failure_preserves_scene_and_packets",
              document_->generation() == generation && meshes_ == old);
  mesh_stage_ = 1;
  smoke_check("async_save_admitted",
              start_persistence(desktop::PersistenceKind::Save, mesh_smoke_path("mesh.scene.json")));
}
// Stage 1: the mesh inspector fits the properties column; the layout capture, then the async cook admission.
void Editor::mesh_smoke_layout() {
  auto *scroll = properties_scroll();
  if (!(scroll && scroll->horizontalScrollBar()->maximum() == 0))
    std::fprintf(stderr, "RACE: properties column: scroll=%d range=%d viewport=%d body_min=%d\n",
                 scroll ? 1 : 0, scroll ? scroll->horizontalScrollBar()->maximum() : -1,
                 scroll ? scroll->viewport()->width() : -1,
                 scroll && scroll->widget() ? scroll->widget()->minimumSizeHint().width() : -1);
  smoke_check("mesh_inspector_fits_width", scroll && scroll->horizontalScrollBar()->maximum() == 0);
  smoke_check("mesh_layout_only_capture", grab().save(mesh_smoke_path("mesh-layout-only.png")));
  mesh_stage_ = 2;
  smoke_check("async_cook_admitted",
              start_persistence(desktop::PersistenceKind::Cook, mesh_smoke_path("mesh.rscene")));
}
// Stage 4: the invalid-dependency scene (or, for a large mesh, an over-budget one) is written and the add
// budget is checked, then its load is admitted.
void Editor::mesh_smoke_invalid_scenes() {
  auto invalid = document_->snapshot();
  if (invalid) {
    for (auto &object : invalid.value().objects)
      for (auto &component : object.components)
        if (component.type == desktop::mesh_type)
          for (auto &field : component.fields)
            if (field.id == 1)
              field.value = PersistentId{0xdeadbeef, 0xfeedbeef};
    smoke_check("invalid_dependency_scene_written",
                check(desktop::Document::write(*services_.files,
                                               path_string(mesh_smoke_path("wrong-asset.scene.json")),
                                               invalid.value(), *document_->schemas(), false)));
  }
  smoke_expected_persistence_error_ = ErrorCode::InvalidInput;
  if (!meshes_->entries().empty() &&
      meshes_->entries().front().geometry->indices().size() > render::viewport_mesh_frame_indices / 4) {
    auto oversized = document_->snapshot();
    if (oversized) {
      const auto it = std::find_if(
          oversized.value().objects.begin(), oversized.value().objects.end(), [](const auto &object) {
            return std::any_of(object.components.begin(), object.components.end(),
                               [](const auto &c) { return c.type == desktop::mesh_type; });
          });
      if (it != oversized.value().objects.end()) {
        auto copy = *it;
        for (std::uint64_t n = 1; n <= 3; ++n) {
          copy.id = {0x627564676574, n};
          oversized.value().objects.push_back(copy);
        }
        smoke_check("over_budget_scene_written",
                    check(desktop::Document::write(*services_.files,
                                                   path_string(mesh_smoke_path("wrong-asset.scene.json")),
                                                   oversized.value(), *document_->schemas(), false)));
        smoke_expected_persistence_error_ = ErrorCode::Exhausted;
      }
    }
    const auto product = assets_path_ + "/" +
                         QString::fromStdString(assets::hex(meshes_->entries().front().reference.revision)) +
                         ".rasset";
    smoke_check("within_budget_instances_added", add_native_mesh(product) && add_native_mesh(product));
    const auto generation = document_->generation();
    const auto packets = meshes_;
    smoke_rejection("over_budget_add_rejected", [&] { add_native_mesh(product); });
    smoke_check("budget_rejection_preserves_history", document_->generation() == generation &&
                                                          meshes_ == packets && bool(document_->undo()) &&
                                                          bool(document_->undo()));
    refresh();
  }
  smoke_generation_ = document_->generation();
  mesh_stage_ = 5;
  smoke_check("bad_dependency_load_admitted",
              start_persistence(desktop::PersistenceKind::Load, mesh_smoke_path("wrong-asset.scene.json")));
}
// Stage 5: the bad load preserved the scene; cooked references, scene, control and Play captures, and Play
// leaves the cooked bytes unchanged; then the receipt with these renderer stats.
void Editor::mesh_smoke_captures() {
  mesh_stage_ = 6;
  persistence_smoke_stage_ = 0;
  smoke_check("bad_dependency_preserves_scene",
              document_->generation() == smoke_generation_ && !smoke_expected_persistence_error_);
  auto snapshot = document_->snapshot();
  auto refs = snapshot ? desktop::mesh_references(snapshot.value())
                       : Result<std::vector<desktop::MeshReference>>{snapshot.error()};
  smoke_check("async_cooked_full_references", refs && refs.value() == mesh_expected_);
  auto *renderer = viewport_->renderer();
  smoke_check("capture_resize", check(renderer->resize(640, 480)));
  auto frame = desktop::scene_frame(*document_, meshes_.get(), {});
  smoke_check("owned_frame", bool(frame));
  if (frame) {
    smoke_check("mesh_draw", check(renderer->draw(frame.value())));
    smoke_check("mesh_capture", check(renderer->capture(native_path(mesh_smoke_path("scene.ppm")))));
    auto control = frame.value();
    for (auto &draw : control.draws)
      draw.geometry.reset();
    smoke_check("control_draw", check(renderer->draw(control)));
    smoke_check("control_capture",
                check(renderer->capture(native_path(mesh_smoke_path("cube-control.ppm")))));
  }
  smoke_check("play_clone", check(document_->start_play()));
  smoke_check("play_tick", check(document_->tick_play(1.0 / 60)));
  auto play = desktop::scene_frame(*document_, meshes_.get(), {}, true, 1);
  smoke_check("play_frame", bool(play));
  if (play) {
    smoke_check("play_draw", check(renderer->draw(play.value())));
    smoke_check("play_capture", check(renderer->capture(native_path(mesh_smoke_path("play.ppm")))));
  }
  smoke_check("stop_play", check(document_->stop_play()));
  smoke_check("after_play_cook",
              check(document_->cook(*services_.files, path_string(mesh_smoke_path("after-play.rscene")))));
  auto before = services_.files->read(path_string(mesh_smoke_path("mesh.rscene")), 64 * 1024 * 1024),
       after = services_.files->read(path_string(mesh_smoke_path("after-play.rscene")), 64 * 1024 * 1024);
  smoke_check("play_preserves_authoring", before && after && before.value() == after.value());
  auto s = renderer->stats();
  smoke_check("hardware_fence_and_debug", s.hardware && !s.device_lost && !s.debug_errors &&
                                              !s.debug_warnings && s.completed_frames == s.submitted_frames &&
                                              s.width == 640 && s.height == 480);
  mesh_smoke_finish(s);
}
// The mesh, material and texture entries, the new-scene release check, then the receipt and exit.
void Editor::mesh_smoke_finish(const render::ViewportStats &s) {
  QJsonArray entries;
  for (const auto &e : meshes_->entries())
    entries.append(QJsonObject{{"asset", QString::fromStdString(assets::id_text(e.reference.asset))},
                               {"revision", QString::fromStdString(assets::hex(e.reference.revision))},
                               {"vertices", qint64(e.geometry->vertices().size())},
                               {"indices", qint64(e.geometry->indices().size())}});
  QJsonArray material_entries, texture_entries;
  for (const auto &e : meshes_->materials())
    material_entries.append(
        QJsonObject{{"asset", QString::fromStdString(assets::id_text(e.reference.asset))},
                    {"revision", QString::fromStdString(assets::hex(e.reference.revision))},
                    {"sampling", e.material->sampling() == render::TextureSampling::LinearClampMips
                                     ? "linear-clamp-mips"
                                     : "linear-clamp-mip0"},
                    {"texture_role", e.material->texture_role() == render::TextureRole::DiffuseReflectanceR
                                         ? "diffuse-reflectance-r"
                                         : "color"}});
  for (const auto &e : meshes_->textures())
    texture_entries.append(QJsonObject{
        {"asset", QString::fromStdString(assets::id_text(e.reference.asset))},
        {"revision", QString::fromStdString(assets::hex(e.reference.revision))},
        {"width", int(e.texture->width())},
        {"height", int(e.texture->height())},
        {"mip_count", int(e.texture->mip_count())},
        {"texel_bytes", qint64(e.texture->texels().size())},
        {"encoding",
         e.texture->encoding() == render::TextureEncoding::LinearData ? "linear-data" : "srgb-color"}});
  std::weak_ptr<platform::RootedFiles> old_source = meshes_->files();
  smoke_check("new_releases_old_meshes",
              new_scene() && meshes_->entries().empty() && !meshes_->files() && old_source.expired());
  smoke_check("new_can_switch_empty_assets_root", choose_assets(smoke_));
  bool passed = true;
  for (const auto &entry : smoke_checks_) {
    auto o = entry.toObject();
    if (o.contains("error") || !o.value("passed").toBool())
      passed = false;
  }
  QJsonObject receipt{{"schema", 1},
                      {"application", "Qt"},
                      {"passed", passed},
                      {"checks", smoke_checks_},
                      {"content_metrics", content_metrics_},
                      {"meshes", entries},
                      {"materials", material_entries},
                      {"textures", texture_entries},
                      {"texture_uploads", qint64(s.texture_uploads)},
                      {"texture_upload_bytes", qint64(s.texture_upload_bytes)},
                      {"texture_cache_hits", qint64(s.texture_cache_hits)},
                      {"scene_source", initial_scene_path_},
                      {"scene_source_kind", initial_scene_source_},
                      {"scene_load_ms", scene_load_ms_},
                      {"synchronous_package_read_ms", package_sync_ms_},
                      {"interactive_package_completion_ms", package_async_ms_},
                      {"interactive_package_admission_ms", package_admission_ms_},
                      {"max_callback_gap_while_package_retained_ms", package_max_callback_gap_ms_},
                      {"successful_package_worker_ms", package_worker_ms_},
                      {"callbacks_while_package_request_retained", qint64(package_pending_heartbeats_)},
                      {"package_scene_id", package_scene_id_},
                      {"package_scene_revision", package_scene_revision_},
                      {"adapter", QString::fromStdString(s.adapter)},
                      {"hardware", s.hardware},
                      {"debug_layer", s.debug_layer},
                      {"debug_errors", qint64(s.debug_errors)},
                      {"debug_warnings", qint64(s.debug_warnings)},
                      {"width", int(s.width)},
                      {"height", int(s.height)},
                      {"source_commit", QString::fromUtf8(build::source_commit)},
                      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)}};
  QSaveFile file(mesh_smoke_path("mesh-receipt.json"));
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(receipt).toJson()) < 0 || !file.commit())
    passed = false;
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(passed ? 0 : 2);
}
} // namespace race_editor
