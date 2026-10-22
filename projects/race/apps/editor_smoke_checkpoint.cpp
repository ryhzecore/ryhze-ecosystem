// Opt-in native checkpoint smoke: mesh checkpoint update requests, cancellation, history and the loose-root
// control, and the checkpoint receipt that the authoring, browser and content smokes also write.
#include "editor_window.hpp"

namespace race_editor {
void Editor::checkpoint_capture(const QString &leaf) {
  auto *renderer = viewport_->renderer();
  smoke_check("checkpoint_capture_resize", check(renderer->resize(640, 480)));
  auto frame = desktop::scene_frame(*document_, meshes_.get(), {});
  smoke_check("checkpoint_frame", bool(frame));
  if (frame) {
    smoke_check("checkpoint_draw", check(renderer->draw(frame.value())));
    smoke_check("checkpoint_capture", check(renderer->capture(native_path(smoke_ + "/" + leaf))));
  }
}
// The checkpoint smoke, one stage per frame while no worker is busy. Stage 0 hands over to the authoring
// (90-93) or browser (99-109) stages, which return to stage 1; stages 1-7 run in the order 1, 11, 2, 3, 30,
// 4, 5, 6, 7.
void Editor::checkpoint_smoke_step() {
  if (smoke_browser_click_ || material_stage_ == -1 || frames_ < 3 || !viewport_->renderer() ||
      persistence_->busy() || mesh_checkpoint_.busy() || project_browser_.busy() || mesh_reimport_.busy() ||
      surface_reimport_.busy() || surface_checkpoint_.busy() || project_authoring_.busy() ||
      material_assignment_.busy())
    return;
  if (checkpoint_stage_ == 0) {
    checkpoint_smoke_setup();
    return;
  }
  if (checkpoint_stage_ >= 90 && checkpoint_stage_ < 99) {
    authoring_smoke_step();
    return;
  }
  if (checkpoint_stage_ >= 99) {
    browser_smoke_step();
    return;
  }
  if (checkpoint_stage_ == 1 || checkpoint_stage_ == 11 || checkpoint_stage_ == 2 || checkpoint_stage_ == 3 ||
      checkpoint_stage_ == 30) {
    checkpoint_smoke_cancelled_requests();
    return;
  }
  if (checkpoint_stage_ == 4) {
    checkpoint_smoke_update();
    return;
  }
  if (checkpoint_stage_ == 5) {
    checkpoint_smoke_loose_control();
    return;
  }
  if (checkpoint_stage_ == 6) {
    smoke_check("checkpoint_missing_preserves", !checkpoint_expected_error_ &&
                                                    document_->generation() == checkpoint_generation_ &&
                                                    current_path_ == checkpoint_authoring_path_);
    checkpoint_expected_error_ = ErrorCode::Cancelled;
    smoke_check("checkpoint_history_request", checkpoint_next(7));
    smoke_check("checkpoint_history_clear", clear_scene_history());
    return;
  }
  if (checkpoint_stage_ != 7)
    return;
  checkpoint_stage_ = 8;
  smoke_check("checkpoint_history_clear_cancels", !checkpoint_expected_error_ &&
                                                      document_->generation() == checkpoint_generation_ &&
                                                      !document_->undo() && !document_->redo());
  smoke_check("checkpoint_layout_capture", grab().save(smoke_ + "/checkpoint-layout-only.png"));
  checkpoint_smoke_finish();
}
// Enters the given stage and requests an update from the selected browser checkpoint.
bool Editor::checkpoint_next(int stage) {
  checkpoint_stage_ = stage;
  return update_from_browser(checkpoint_file_);
}
// Stage 0: two instances of the mesh and, for the UV test, one shared material; then an edit, the initial
// save and capture, and the docked content browser.
void Editor::checkpoint_smoke_setup() {
  if (checkpoint_material_stage_ == 0) {
    smoke_check("checkpoint_add_first", add_native_mesh(assets_path_ + "/" + mesh_product_));
    checkpoint_other_ = selection_;
    smoke_check("checkpoint_add_second", add_native_mesh(assets_path_ + "/" + mesh_product_));
    checkpoint_selected_ = selection_;
  }
  if (!selected_row() || !selected_row()->mesh) {
    checkpoint_smoke_finish();
    return;
  }
  if (authoring_uv_test_ && !material_product_.isEmpty() && checkpoint_material_stage_ < 3) {
    const auto assign = [&] {
      auto *button = findChild<QToolButton *>("UiAssignMaterial");
      auto *scroll = properties_scroll();
      if (scroll && button)
        scroll->ensureWidgetVisible(button);
      smoke_check("checkpoint_material_button", button && button->isEnabled() && button->isVisible());
      material_stage_ = -1;
      if (button)
        button->click();
      material_stage_ = 0;
      smoke_check("checkpoint_material_worker_admitted", material_assignment_.busy());
    };
    if (checkpoint_material_stage_ == 0) {
      checkpoint_material_stage_ = 1;
      assign();
      return;
    }
    smoke_check("checkpoint_material_assigned",
                selected_row()->material.has_value() && !meshes_->materials().empty());
    if (!selected_row()->material) {
      checkpoint_smoke_finish();
      return;
    }
    if (checkpoint_material_stage_ == 1) {
      checkpoint_material_ = selected_row()->material;
      checkpoint_material_stage_ = 2;
      smoke_check("checkpoint_material_select_other", select_object(checkpoint_other_));
      assign();
      return;
    }
    smoke_check("checkpoint_material_same_reference", selected_row()->material == checkpoint_material_);
    smoke_check("checkpoint_material_select_target", select_object(checkpoint_selected_));
    checkpoint_material_stage_ = 3;
  }
  auto row = *selected_row();
  row.local.translation.x = 2;
  row.color = {.8, .3, .2};
  smoke_check("checkpoint_prior_edit", check(document_->edit(row, document_->generation())));
  refresh();
  checkpoint_before_ = *selected_row()->mesh;
  checkpoint_authoring_path_ = smoke_ + "/authored.scene.json";
  smoke_check("checkpoint_save_initial",
              check(document_->save(*services_.files, path_string(checkpoint_authoring_path_))));
  current_path_ = checkpoint_authoring_path_;
  dirty_ = false;
  checkpoint_generation_ = document_->generation();
  checkpoint_capture("before.ppm");
  content_dock_->show();
  content_dock_->raise();
  resizeDocks({content_dock_}, {240}, Qt::Vertical);
  checkpoint_stage_ = authoring_test_ ? 90 : 99;
}
// Stages 1, 11, 2, 3 and 30: update requests cancelled by a selection A-B-A, a browser row A-B-A, typing, a
// drag and a step A-B-A each preserve the scene; stage 30 then admits the browser's update.
void Editor::checkpoint_smoke_cancelled_requests() {
  if (checkpoint_stage_ == 1) {
    smoke_check("checkpoint_selection_cancel_preserves",
                !checkpoint_expected_error_ && document_->generation() == checkpoint_generation_ &&
                    *selected_row()->mesh == checkpoint_before_ && !dirty_ &&
                    current_path_ == checkpoint_authoring_path_);
    checkpoint_expected_error_ = ErrorCode::Cancelled;
    smoke_check("checkpoint_browser_selection_request", checkpoint_next(11));
    auto *tree = content_browser_->findChild<QListWidget *>("ContentRows");
    for (auto value : {Qt::Key_Down, Qt::Key_Up}) {
      QKeyEvent press(QEvent::KeyPress, value, Qt::NoModifier),
          release(QEvent::KeyRelease, value, Qt::NoModifier);
      QCoreApplication::sendEvent(tree, &press);
      QCoreApplication::sendEvent(tree, &release);
    }
    return;
  }
  if (checkpoint_stage_ == 11) {
    smoke_check("checkpoint_browser_selection_aba_cancels",
                !checkpoint_expected_error_ && document_->generation() == checkpoint_generation_ &&
                    *selected_row()->mesh == checkpoint_before_ && !dirty_);
    checkpoint_expected_error_ = ErrorCode::Cancelled;
    smoke_check("checkpoint_typing_request", checkpoint_next(2));
    smoke_type("-");
    smoke_pending_text_ = fields_[0][0]->text();
    return;
  }
  if (checkpoint_stage_ == 2) {
    smoke_check("checkpoint_pending_text_preserved", !checkpoint_expected_error_ &&
                                                         fields_[0][0]->text() == smoke_pending_text_ &&
                                                         !fields_[0][0]->hasAcceptableInput() &&
                                                         document_->generation() == checkpoint_generation_);
    refresh_inspector();
    viewport_->frame_selected();
    checkpoint_expected_error_ = ErrorCode::Cancelled;
    smoke_check("checkpoint_drag_request", checkpoint_next(3));
    smoke_check("checkpoint_drag_preview", viewport_->smoke_drag(true));
    return;
  }
  if (checkpoint_stage_ == 3) {
    smoke_check("checkpoint_drag_cancel_preserves", !checkpoint_expected_error_ &&
                                                        document_->generation() == checkpoint_generation_ &&
                                                        *selected_row()->mesh == checkpoint_before_);
    checkpoint_expected_error_ = ErrorCode::Cancelled;
    smoke_check("checkpoint_step_request", checkpoint_next(30));
    const auto text = fields_[0][0]->text();
    fields_[0][0]->stepUp();
    fields_[0][0]->stepDown();
    smoke_check("checkpoint_step_aba_text", fields_[0][0]->text() == text);
    return;
  }
  if (checkpoint_stage_ == 30) {
    smoke_check("checkpoint_step_cancel_preserves", !checkpoint_expected_error_ &&
                                                        document_->generation() == checkpoint_generation_ &&
                                                        *selected_row()->mesh == checkpoint_before_);
    checkpoint_stage_ = 4;
    browser_button("Update selected");
    smoke_check("checkpoint_update_request", mesh_checkpoint_.busy());
    smoke_check("checkpoint_persistence_exclusion",
                !start_persistence(desktop::PersistenceKind::Save, smoke_ + "/busy.json"));
    return;
  }
}
// Stage 4: the update changes only the selected instance; captures, save, cook and undo/redo keep the pins,
// then the same-revision request.
void Editor::checkpoint_smoke_update() {
  smoke_check("checkpoint_update_complete",
              checkpoint_completion_ && checkpoint_completion_->result && checkpoint_completion_->changed);
  smoke_check("checkpoint_selected_only", selected_row() && selected_row()->mesh &&
                                              selected_row()->mesh->asset == checkpoint_before_.asset &&
                                              *selected_row()->mesh != checkpoint_before_ &&
                                              selection_ == checkpoint_selected_);
  for (const auto &row : rows_)
    if (row.id == checkpoint_other_) {
      smoke_check("checkpoint_other_instance_pinned", row.mesh == checkpoint_before_);
      if (checkpoint_material_)
        smoke_check("checkpoint_other_material_pinned", row.material == checkpoint_material_);
    }
  if (checkpoint_material_)
    smoke_check("checkpoint_updated_material_pinned", selected_row()->material == checkpoint_material_);
  smoke_check("checkpoint_fields_and_path_preserved",
              selected_row()->local.translation.x == 2 && selected_row()->color == scene::Vec3{.8, .3, .2} &&
                  current_path_ == checkpoint_authoring_path_ && dirty_);
  browser_check_revisions("updated", 2);
  auto *details = content_browser_->findChild<QScrollArea *>("ContentDetailsScroll");
  auto *resident = content_browser_->findChild<QPlainTextEdit *>("ContentResident");
  details->ensureWidgetVisible(resident);
  smoke_check("browser_resident_capture", grab().save(smoke_ + "/browser-revisions-layout-only.png"));
  checkpoint_capture("updated.ppm");
  smoke_check("checkpoint_saved",
              check(document_->save(*services_.files, path_string(smoke_ + "/updated.scene.json"))));
  smoke_check("checkpoint_cooked",
              check(document_->cook(*services_.files, path_string(smoke_ + "/updated.rscene"))));
  smoke_check("checkpoint_undo", check(document_->undo()));
  refresh();
  checkpoint_capture("undo.ppm");
  smoke_check("checkpoint_undo_reference", *selected_row()->mesh == checkpoint_before_);
  browser_check_revisions("undo", 2);
  if (checkpoint_material_)
    smoke_check("checkpoint_undo_material_pinned", selected_row()->material == checkpoint_material_);
  smoke_check("checkpoint_prior_history_survives", check(document_->undo()));
  refresh();
  smoke_check("checkpoint_prior_transform_restored", selected_row()->local.translation.x == 0);
  smoke_check("checkpoint_prior_edit_redo", check(document_->redo()));
  smoke_check("checkpoint_update_redo", check(document_->redo()));
  refresh();
  checkpoint_capture("redo.ppm");
  browser_check_revisions("redo", 2);
  if (checkpoint_material_)
    smoke_check("checkpoint_redo_material_pinned", selected_row()->material == checkpoint_material_);
  checkpoint_generation_ = document_->generation();
  smoke_check("checkpoint_noop_request", checkpoint_next(5));
}
// Stage 5: the same revision is a no-op; a standalone document prepares the cooked scene from the loose
// root and is captured, then a missing checkpoint is requested.
void Editor::checkpoint_smoke_loose_control() {
  smoke_check("checkpoint_same_revision_noop", checkpoint_completion_ && checkpoint_completion_->result &&
                                                   !checkpoint_completion_->changed &&
                                                   document_->generation() == checkpoint_generation_);
  // Validate a standalone loose-root decode using the persisted product pins.
  auto control = desktop::Document::create();
  smoke_check("checkpoint_control_document", bool(control));
  if (control) {
    smoke_check("checkpoint_control_load",
                check(control.value()->load(*services_.files, path_string(smoke_ + "/updated.rscene"))));
    auto snapshot = control.value()->snapshot();
    auto refs = snapshot ? desktop::mesh_references(snapshot.value())
                         : Result<std::vector<desktop::MeshReference>>{snapshot.error()};
    auto materials = snapshot ? desktop::material_references(snapshot.value())
                              : Result<std::vector<desktop::MaterialReference>>{snapshot.error()};
    auto prepared =
        refs && materials
            ? desktop::MeshSet::prepare(meshes_->files(), refs.value(), {}, materials.value())
            : Result<std::shared_ptr<const desktop::MeshSet>>{!refs ? refs.error() : materials.error()};
    smoke_check("checkpoint_explicit_loose_preparation", bool(prepared));
    if (checkpoint_material_)
      smoke_check("checkpoint_loose_material_pinned",
                  materials && materials.value().size() == 2 &&
                      std::all_of(materials.value().begin(), materials.value().end(),
                                  [&](const auto &ref) { return ref == *checkpoint_material_; }));
    if (prepared) {
      auto frame = desktop::scene_frame(*control.value(), prepared.value().get(), {});
      smoke_check("checkpoint_loose_frame", bool(frame));
      if (frame) {
        auto *renderer = viewport_->renderer();
        smoke_check("checkpoint_loose_draw",
                    check(renderer->resize(640, 480)) && check(renderer->draw(frame.value())));
        smoke_check("checkpoint_loose_capture",
                    check(renderer->capture(native_path(smoke_ + "/loose-control.ppm"))));
      }
    }
  }
  checkpoint_expected_error_ = ErrorCode::NotFound;
  checkpoint_stage_ = 6;
  smoke_check("checkpoint_missing_request",
              start_mesh_update(checkpoint_project_, QFileInfo(checkpoint_file_).absolutePath() + "/" +
                                                         QString(64, 'f') + ".json"));
}
void Editor::checkpoint_smoke_finish() {
  if (closing_smoke_)
    return;
  const auto stats = viewport_->renderer()->stats();
  smoke_check("checkpoint_hardware", stats.hardware && stats.debug_layer && !stats.debug_errors &&
                                         !stats.debug_warnings && !stats.device_lost);
  bool passed = true;
  for (const auto &entry : smoke_checks_) {
    auto o = entry.toObject();
    if (o.contains("error") || !o.value("passed").toBool())
      passed = false;
  }
  QJsonObject receipt{
      {"schema", 1},
      {"passed", passed},
      {"checks", smoke_checks_},
      {"measurements", checkpoint_metrics_},
      {"authoring_measurements", authoring_metrics_},
      {"authoring_checks", authoring_checks_},
      {"browser_measurements", browser_metrics_},
      {"reimport_measurements", reimport_metrics_},
      {"hardware", stats.hardware},
      {"debug_layer", stats.debug_layer},
      {"debug_errors", qint64(stats.debug_errors)},
      {"debug_warnings", qint64(stats.debug_warnings)},
      {"adapter", QString::fromStdString(stats.adapter)},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"scope",
       "Actual Qt event handlers and hardware D3D12 readbacks; synthetic input, no physical input latency claim"}};
  QSaveFile file(smoke_ + "/checkpoint-receipt.json");
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(receipt).toJson()) < 0 || !file.commit())
    passed = false;
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(passed ? 0 : 2);
}
} // namespace race_editor
