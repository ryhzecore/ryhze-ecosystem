// Opt-in native render smokes: Qt surface recreation across device epochs, and P03.04 light/output.
#include "editor_window.hpp"

namespace race_editor {
// Scene JSON of the current document for the epoch and light smokes. It throws, so the step's catch reports
// the failure and finishes the run.
std::string Editor::smoke_scene_json() {
  auto snapshot = document_->snapshot();
  if (!snapshot)
    throw std::runtime_error(snapshot.error().message);
  auto result = scene::encode_json(snapshot.value(), *document_->schemas(), {});
  if (!result)
    throw std::runtime_error(result.error().message);
  return result.value();
}
void Editor::epoch_smoke_step() {
  if (frames_ < 12 || !viewport_->renderer())
    return;
  try {
    if (epoch_stage_ == 0) {
      add_box();
      epoch_original_ = smoke_scene_json();
      auto row = *selected_row();
      row.local.translation.x = 1.25;
      if (!check(document_->edit(row, document_->generation())))
        throw std::runtime_error("epoch edit fixture failed");
      changed();
      viewport_->frame_selected();
      viewport_->tool = 0;
      viewport_->axis = 0;
      epoch_authored_ = smoke_scene_json();
      epoch_stage_ = 1;
      return;
    }
    if (epoch_stage_ == 1) {
      epoch_smoke_retire_surface();
      return;
    }
    if (epoch_stage_ == 2)
      epoch_smoke_restore_surface();
  } catch (const std::exception &e) {
    error(e.what());
    epoch_stage_ = 3;
  }
  if (epoch_stage_ != 3)
    return;
  epoch_smoke_finish();
}
// Stage 1: retire the real Qt surface under a pending gizmo preview; the owner and authority must survive.
void Editor::epoch_smoke_retire_surface() {
  epoch_owner_ = viewport_->renderer();
  epoch_number_ = epoch_owner_->stats().device_epoch;
  epoch_generation_ = document_->generation();
  epoch_selection_ = selection_;
  epoch_camera_ = viewport_->camera_pose();
  smoke_check("epoch_before_capture", bool(epoch_owner_->capture(native_path(smoke_ + "/before.ppm"))));
  auto token = epoch_owner_->image_token();
  if (!token)
    throw std::runtime_error(token.error().message);
  epoch_token_ = token.value();
  smoke_check("epoch_pending_gizmo_is_unpublished", viewport_->smoke_drag(false, true, 0) &&
                                                        viewport_->previewing() &&
                                                        smoke_scene_json() == epoch_authored_);
  viewport_->destroy(); // Real Qt SurfaceAboutToBeDestroyed event, before HWND destruction.
  smoke_check("qt_surface_retirement_keeps_owner_and_cancels_preview",
              viewport_->renderer() == epoch_owner_ &&
                  epoch_owner_->state() == render::ViewportState::SurfaceRetired && !viewport_->previewing());
  smoke_check("qt_surface_retirement_preserves_authority",
              smoke_scene_json() == epoch_authored_ && document_->generation() == epoch_generation_ &&
                  selection_ == epoch_selection_ && viewport_->camera_pose() == epoch_camera_);
  viewport_->create();
  viewport_->show();
  epoch_stage_ = 2;
}
// Stage 2: once restored, compare the epoch, pixels and undo/redo, then restore during Play.
void Editor::epoch_smoke_restore_surface() {
  const auto same_image = [this](const QString &a, const QString &b) {
    auto left = services_.files->read(path_string(smoke_ + a), 64u * 1024u * 1024u);
    auto right = services_.files->read(path_string(smoke_ + b), 64u * 1024u * 1024u);
    return left && right && left.value() == right.value();
  };
  if (epoch_owner_->state() != render::ViewportState::Ready || !epoch_owner_->stats().has_image)
    return;
  const auto restored = epoch_owner_->stats();
  smoke_check("qt_surface_restore_advances_same_owner_epoch",
              viewport_->renderer() == epoch_owner_ && restored.device_epoch == epoch_number_ + 1 &&
                  restored.recreation == render::ViewportRecreation::SurfaceRestore &&
                  restored.retired.completed_frames == restored.retired.submitted_frames);
  smoke_check("qt_surface_restore_preserves_authority",
              smoke_scene_json() == epoch_authored_ && document_->generation() == epoch_generation_ &&
                  selection_ == epoch_selection_ && viewport_->selected == epoch_selection_ &&
                  viewport_->camera_pose() == epoch_camera_);
  auto stale = epoch_owner_->capture(native_path(smoke_ + "/stale.ppm"), *epoch_token_);
  smoke_check("qt_old_epoch_capture_rejects_before_output",
              !stale && stale.error().code == ErrorCode::StaleHandle &&
                  !QFileInfo::exists(smoke_ + "/stale.ppm"));
  smoke_check("qt_restored_capture", bool(epoch_owner_->capture(native_path(smoke_ + "/restored.ppm"))));
  smoke_check("qt_surface_restore_pixels_exact", same_image("/before.ppm", "/restored.ppm"));
  smoke_check("qt_surface_restore_keeps_undo",
              bool(document_->undo()) && smoke_scene_json() == epoch_original_);
  smoke_check("qt_surface_restore_keeps_redo",
              bool(document_->redo()) && smoke_scene_json() == epoch_authored_);
  refresh();
  if (!check(document_->start_play()) || !check(document_->tick_play(1.0 / 60)))
    throw std::runtime_error("epoch Play fixture failed");
  viewport_->simulation_tick = 1;
  const auto play_generation = document_->generation(true);
  smoke_check("qt_play_before_render",
              viewport_->render() && bool(epoch_owner_->capture(native_path(smoke_ + "/play-before.ppm"))));
  if (!check(epoch_owner_->retire_surface()))
    throw std::runtime_error("epoch Play retirement failed");
  play_accumulator_ = .049;
  smoke_check("qt_play_restore_same_tick",
              viewport_->render() && document_->playing() && document_->generation(true) == play_generation &&
                  epoch_owner_->stats().simulation_tick == 1 && play_accumulator_ == 0 &&
                  viewport_->camera_pose() == epoch_camera_);
  smoke_check("qt_play_restored_capture",
              bool(epoch_owner_->capture(native_path(smoke_ + "/play-restored.ppm"))));
  smoke_check("qt_play_restore_pixels_exact", same_image("/play-before.ppm", "/play-restored.ppm"));
  smoke_check("qt_play_stop_keeps_authoring",
              bool(document_->stop_play()) && smoke_scene_json() == epoch_authored_);
  epoch_stage_ = 3;
}
void Editor::epoch_smoke_finish() {
  const auto stats = viewport_->renderer()->stats();
  bool passed = true;
  for (const auto &item : smoke_checks_) {
    const auto check = item.toObject();
    if (check.contains("error") || !check.value("passed").toBool())
      passed = false;
  }
  passed = passed && stats.hardware && stats.debug_layer && !stats.debug_errors && !stats.debug_warnings &&
           !stats.diagnostics_incomplete;
  QJsonObject receipt{
      {"schema", 1},
      {"application", "RACE Qt epoch smoke"},
      {"passed", passed},
      {"checks", smoke_checks_},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"adapter", QString::fromStdString(stats.adapter)},
      {"device_epoch", qint64(stats.device_epoch)},
      {"debug_errors", qint64(stats.debug_errors)},
      {"debug_warnings", qint64(stats.debug_warnings)},
      {"scope",
       "Real Qt surface recreation and controlled Play restore; no programmatic or spontaneous device removal"}};
  QSaveFile file(smoke_ + "/epoch-receipt.json");
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(receipt).toJson()) < 0 || !file.commit())
    passed = false;
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(passed ? 0 : 2);
}
void Editor::light_smoke_step() {
  if (frames_ < 12 || !viewport_->renderer())
    return;
  try {
    if (light_stage_ == 0) {
      light_smoke_physical_controls();
      return;
    }
    if (light_stage_ == 1) {
      light_smoke_undo_redo();
      return;
    }
    if (light_stage_ == 2) {
      if (persistence_->busy())
        return;
      light_stage_ = 3;
      if (!start_persistence(desktop::PersistenceKind::Cook, smoke_ + "/lighting.rscene"))
        throw std::runtime_error("light smoke cook admission failed");
      return;
    }
    if (light_stage_ == 3) {
      if (persistence_->busy())
        return;
      smoke_check("light_saved_files_exist", QFileInfo::exists(smoke_ + "/lighting.scene.json") &&
                                                 QFileInfo::exists(smoke_ + "/lighting.rscene"));
      if (!new_scene())
        throw std::runtime_error("light smoke new scene failed");
      auto fresh = document_->lighting();
      smoke_check("light_new_scene_is_legacy",
                  bool(fresh) && !fresh.value().output && !physical_toggle_->isChecked());
      light_stage_ = 4;
      if (!start_persistence(desktop::PersistenceKind::Load, smoke_ + "/lighting.scene.json"))
        throw std::runtime_error("light smoke JSON load admission failed");
      return;
    }
    if (light_stage_ == 4) {
      if (persistence_->busy())
        return;
      smoke_check("light_json_reload_preserves_settings",
                  smoke_scene_json() == light_physical_ && physical_toggle_->isChecked() &&
                      light_fields_[0]->value() == 1250 && light_fields_[2]->value() == 1.5);
      if (!new_scene())
        throw std::runtime_error("light smoke second new scene failed");
      light_stage_ = 5;
      if (!start_persistence(desktop::PersistenceKind::Load, smoke_ + "/lighting.rscene"))
        throw std::runtime_error("light smoke cooked load admission failed");
      return;
    }
    if (light_stage_ == 5)
      light_smoke_play();
  } catch (const std::exception &e) {
    error(e.what());
    light_stage_ = 6;
  }
  if (light_stage_ != 6)
    return;
  light_smoke_finish();
}
// Stage 0: legacy frame and capture, then the real toggle and typed values publish the physical settings.
void Editor::light_smoke_physical_controls() {
  const auto commit = [this](std::size_t field, double value) {
    light_fields_[field]->setValue(value);
    emit light_fields_[field]->editingFinished();
  };
  auto *renderer = viewport_->renderer();
  light_legacy_ = smoke_scene_json();
  smoke_check("light_controls_present", physical_toggle_ && !physical_toggle_->isChecked() &&
                                            physical_toggle_->isEnabled() && !light_fields_[0]->isEnabled() &&
                                            output_status_->text().startsWith("Output: LegacyViewportV1"));
  const auto legacy = renderer->stats();
  smoke_check("light_legacy_frame", legacy.lighting_profile == render::LightingProfile::LegacyViewportV1 &&
                                        !legacy.physical_output_installed);
  smoke_check("light_legacy_capture", bool(renderer->capture(native_path(smoke_ + "/legacy.ppm"))));
  physical_toggle_->setChecked(true); // Real toggle: publishes the physical defaults as one undo step.
  auto toggled = document_->lighting();
  smoke_check("light_toggle_publishes_physical_defaults",
              bool(toggled) && toggled.value().output &&
                  toggled.value().output->profile == render::LightingProfile::PhysicalDiffuseV1 &&
                  toggled.value().light.has_value() && light_fields_[0]->isEnabled());
  commit(0, 1250);
  commit(2, 1.5);
  commit(7, 2);
  auto typed = document_->lighting();
  smoke_check("light_typed_values_published", bool(typed) && typed.value().light && typed.value().output &&
                                                  typed.value().light->illuminance_lux == 1250 &&
                                                  typed.value().output->exposure_stops == 1.5 &&
                                                  typed.value().output->background_cd_m2.x == 2);
  smoke_check("light_status_reports_physical_output",
              output_status_->text().startsWith("Output: PhysicalDiffuseV1"));
  light_physical_ = smoke_scene_json();
  light_stage_ = 1;
}
// Stage 1: physical frame and captures, undo/redo followed by the controls, then the JSON save admission.
void Editor::light_smoke_undo_redo() {
  auto *renderer = viewport_->renderer();
  const auto stats = renderer->stats();
  smoke_check("light_physical_frame_rendered",
              stats.lighting_profile == render::LightingProfile::PhysicalDiffuseV1 &&
                  stats.physical_output_installed && stats.output_allocation_bytes > 0 &&
                  stats.output_allocation_bytes <= stats.output_budget_bytes &&
                  (stats.draw_graph.operation_mask & (1u << 8)) != 0);
  smoke_check("light_physical_capture", bool(renderer->capture(native_path(smoke_ + "/physical.ppm"))));
  auto token = renderer->image_token();
  if (!token)
    throw std::runtime_error(token.error().message);
  auto scene_linear = renderer->capture_scene_linear(
      native_path(smoke_ + "/physical.pfm"), native_path(smoke_ + "/physical.rgba16f"), token.value());
  smoke_check("light_scene_linear_capture",
              bool(scene_linear) && !scene_linear.value().annotations_included &&
                  std::abs(scene_linear.value().exposure_scale - std::exp2(1.5) / 100.0) < 1e-6);
  bool undone = true;
  for (int i = 0; i < 4; ++i)
    undone = undone && bool(document_->undo());
  smoke_check("light_undo_restores_legacy", undone && smoke_scene_json() == light_legacy_);
  refresh();
  smoke_check("light_controls_follow_undo", !physical_toggle_->isChecked() && !light_fields_[0]->isEnabled());
  bool redone = true;
  for (int i = 0; i < 4; ++i)
    redone = redone && bool(document_->redo());
  smoke_check("light_redo_restores_physical", redone && smoke_scene_json() == light_physical_);
  refresh();
  smoke_check("light_controls_follow_redo", physical_toggle_->isChecked() &&
                                                light_fields_[0]->value() == 1250 &&
                                                light_fields_[2]->value() == 1.5);
  light_stage_ = 2;
  if (!start_persistence(desktop::PersistenceKind::Save, smoke_ + "/lighting.scene.json"))
    throw std::runtime_error("light smoke save admission failed");
}
// Stage 5: cooked reload, controls frozen during Play, a physical Play frame, and Stop keeps authoring.
void Editor::light_smoke_play() {
  if (persistence_->busy())
    return;
  smoke_check("light_cooked_reload_preserves_settings",
              smoke_scene_json() == light_physical_ && physical_toggle_->isChecked());
  if (!check(document_->start_play()))
    throw std::runtime_error("light smoke Play failed");
  refresh_play_controls();
  smoke_check("light_controls_frozen_during_play",
              !physical_toggle_->isEnabled() && !light_fields_[0]->isEnabled());
  viewport_->simulation_tick = simulation_ticks_;
  smoke_check("light_play_frame_physical",
              viewport_->render() && viewport_->renderer()->stats().lighting_profile ==
                                         render::LightingProfile::PhysicalDiffuseV1);
  smoke_check("light_play_stop_keeps_authoring",
              bool(document_->stop_play()) && smoke_scene_json() == light_physical_);
  refresh_play_controls();
  light_stage_ = 6;
}
void Editor::light_smoke_finish() {
  const auto stats = viewport_->renderer()->stats();
  bool passed = true;
  for (const auto &item : smoke_checks_) {
    const auto check = item.toObject();
    if (check.contains("error") || !check.value("passed").toBool())
      passed = false;
  }
  passed = passed && stats.hardware && stats.debug_layer && !stats.debug_errors && !stats.debug_warnings &&
           !stats.diagnostics_incomplete;
  QJsonObject receipt{
      {"schema", 1},
      {"application", "RACE Qt light/output smoke"},
      {"passed", passed},
      {"checks", smoke_checks_},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"adapter", QString::fromStdString(stats.adapter)},
      {"device_epoch", qint64(stats.device_epoch)},
      {"lighting_profile", int(stats.lighting_profile)},
      {"physical_output_installed", stats.physical_output_installed},
      {"output_allocation_bytes", qint64(stats.output_allocation_bytes)},
      {"output_budget_bytes", qint64(stats.output_budget_bytes)},
      {"debug_errors", qint64(stats.debug_errors)},
      {"debug_warnings", qint64(stats.debug_warnings)},
      {"scope",
       "Explicit opt-in physical lighting controls, undo/redo, JSON and cooked persistence through the background worker, Play isolation; SDR display only, no HDR presentation"}};
  QSaveFile file(smoke_ + "/light-receipt.json");
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(receipt).toJson()) < 0 || !file.commit())
    passed = false;
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(passed ? 0 : 2);
}
} // namespace race_editor
