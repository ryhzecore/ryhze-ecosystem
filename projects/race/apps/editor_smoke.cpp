// Opt-in native smoke runs: the shared check helpers and the default authoring/persistence smoke.
#include "editor_window.hpp"

namespace race_editor {
void Editor::smoke_check(QString name, bool passed) {
  smoke_checks_.append(QJsonObject{{"check", name}, {"passed", passed}});
}
bool Editor::smoke_passed() const {
  for (const auto &entry : smoke_checks_) {
    const auto check = entry.toObject();
    if (check.contains("error") || !check.value("passed").toBool())
      return false;
  }
  return true;
}
void Editor::smoke_rejection(QString name, const std::function<void()> &operation) {
  const auto before = smoke_checks_.size();
  operation();
  const bool reported =
      smoke_checks_.size() == before + 1 && smoke_checks_.last().toObject().contains("error");
  if (reported) {
    const auto expected = smoke_checks_.takeAt(before).toObject();
    smoke_checks_.append(
        QJsonObject{{"check", name}, {"passed", true}, {"expected_error", expected.value("error")}});
  } else
    smoke_check(name, false);
}
// The default smoke. Each part holds frame- or stage-gated blocks that are evaluated in this order on every
// frame; a block can advance persistence_smoke_stage_ to a later block's stage within the same frame.
void Editor::smoke_step() {
  smoke_authoring_frames();
  smoke_play_and_file_frames();
  smoke_background_stages();
  smoke_pending_text_stages();
  smoke_preview_stages();
  if (persistence_smoke_stage_ == 18) {
    smoke_intermediate_input_rejections();
    smoke_play_commit_and_checkpoint();
  }
  smoke_checkpoint_stages();
  smoke_recovery_stages();
  if (persistence_smoke_stage_ == 20)
    smoke_finish();
}
// Frames 15-36: keyboard navigation, inspector typing, the initial capture, add/commit/undo/redo, viewport
// drags and the edited capture.
void Editor::smoke_authoring_frames() {
  if (frames_ == 18)
    smoke_check("viewport_keyboard_move_release_focus_repeat", viewport_->smoke_navigation());
  if (frames_ == 36) {
    const auto before = viewport_->camera_pose();
    const auto generation = document_->generation();
    const auto text = fields_[0][0]->text();
    smoke_type("8.2500");
    viewport_->navigate(1.0 / 60);
    smoke_check("inspector_typing_does_not_navigate_or_publish", viewport_->camera_pose() == before &&
                                                                     document_->generation() == generation &&
                                                                     pending_inspector());
    smoke_type(text);
    refresh_inspector();
  }
  if (frames_ == 15) {
    smoke_check("hardware_viewport_created", viewport_->renderer() != nullptr);
    if (viewport_->renderer())
      smoke_check("initial_gpu_capture",
                  bool(viewport_->renderer()->capture(native_path(smoke_ + "/initial.ppm"))));
  }
  if (frames_ == 20) {
    add_action_->trigger();
    auto *row = selected_row();
    smoke_check("toolbar_add_box", row && rows_.size() == 2);
    fields_[0][0]->setValue(2);
    QMetaObject::invokeMethod(fields_[0][0], "editingFinished", Qt::DirectConnection);
    row = selected_row();
    smoke_check("inspector_position_commit", row && row->local.translation.x == 2);
    auto undo = document_->undo();
    refresh();
    row = selected_row();
    smoke_check("undo_transform", bool(undo) && row && row->local.translation.x == 0);
    auto redo = document_->redo();
    refresh();
    row = selected_row();
    smoke_check("redo_transform", bool(redo) && row && row->local.translation.x == 2);
  }
  if (frames_ == 25)
    smoke_check("viewport_escape_discards_preview", viewport_->smoke_drag(true));
  if (frames_ == 28) {
    smoke_check("viewport_drag_one_undo", viewport_->smoke_drag(false));
    refresh();
  }
  if (frames_ == 35 && viewport_->renderer())
    smoke_check("edited_gpu_capture",
                bool(viewport_->renderer()->capture(native_path(smoke_ + "/edited.ppm"))));
}
// Frames 40-100: Play isolation, synchronous save/cook/load and the resized layout and GPU captures.
void Editor::smoke_play_and_file_frames() {
  if (frames_ == 40) {
    play_action_->trigger();
    smoke_check("play_isolated_world", document_->playing());
  }
  if (frames_ == 65) {
    play_action_->trigger();
    smoke_check("stop_restores_authoring_world",
                !document_->playing() && document_->objects().value()[0].local.rotation == scene::Quat{});
  }
  if (frames_ == 70) {
    auto path = smoke_ + "/edited.scene.json";
    smoke_check("save_json", bool(document_->save(*services_.files, path_string(path))));
    smoke_check("cook_rscene",
                bool(document_->cook(*services_.files, path_string(smoke_ + "/edited.rscene"))));
    smoke_check("load_json", open(path) && rows_.size() == 2);
    smoke_check("load_rscene", open(smoke_ + "/edited.rscene") && rows_.size() == 2);
    resize(1100, 720);
  }
  if (frames_ == 100) {
    smoke_check("qt_layout_only_capture_excludes_native_viewport",
                grab().save(smoke_ + "/editor-layout-only.png"));
    if (viewport_->renderer())
      smoke_check("resized_gpu_capture",
                  bool(viewport_->renderer()->capture(native_path(smoke_ + "/resized.ppm"))));
  }
}
// Stages 0, 2 and 4: background save, cook and load admissions, each after the previous one finishes.
void Editor::smoke_background_stages() {
  if (frames_ >= 110 && persistence_smoke_stage_ == 0) {
    persistence_smoke_stage_ = 1;
    smoke_check("background_save_admitted",
                start_persistence(desktop::PersistenceKind::Save, smoke_ + "/background.scene.json"));
  }
  if (persistence_smoke_stage_ == 2 && !persistence_->busy()) {
    persistence_smoke_stage_ = 3;
    smoke_check("background_cook_admitted",
                start_persistence(desktop::PersistenceKind::Cook, smoke_ + "/background.rscene"));
  }
  if (persistence_smoke_stage_ == 4 && !persistence_->busy()) {
    persistence_smoke_stage_ = 5;
    smoke_check("background_load_admitted",
                start_persistence(desktop::PersistenceKind::Load, smoke_ + "/background.rscene"));
  }
}
// Stages 6, 8 and 10: pending inspector text across a save and a load, and the intermediate-input save
// rejection.
void Editor::smoke_pending_text_stages() {
  if (persistence_smoke_stage_ == 6) {
    smoke_check("background_load_published", rows_.size() == 2 && !dirty_ && !document_->playing());
    selection_ = rows_.front().id;
    refresh();
    smoke_generation_ = document_->generation();
    persistence_smoke_stage_ = 7;
    smoke_check("pending_text_save_admitted",
                start_persistence(desktop::PersistenceKind::Save, smoke_ + "/typing.scene.json"));
    after_save_ = [this] { smoke_deferred_action_ = true; };
    smoke_type("12.3456");
    auto *line = fields_[0][0]->findChild<QLineEdit *>();
    line->setSelection(2, 3);
    smoke_pending_text_ = line->text();
    smoke_selected_text_ = line->selectedText();
    smoke_cursor_ = line->cursorPosition();
    smoke_check("typing_does_not_publish_without_enter",
                pending_inspector() && document_->generation() == smoke_generation_);
  }
  if (persistence_smoke_stage_ == 8) {
    auto *line = fields_[0][0]->findChild<QLineEdit *>();
    smoke_check("save_preserves_text_cursor_selection", line->text() == smoke_pending_text_ &&
                                                            line->selectedText() == smoke_selected_text_ &&
                                                            line->cursorPosition() == smoke_cursor_);
    smoke_check("save_does_not_run_destructive_continuation",
                !smoke_deferred_action_ && document_->generation() == smoke_generation_);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(fields_[0][0], &enter);
    smoke_check("preserved_text_commits_on_enter",
                selected_row() && selected_row()->local.translation.x == 12.3456);
    smoke_check("preserved_text_has_undo", bool(document_->undo()));
    refresh();
    smoke_generation_ = document_->generation();
    persistence_smoke_stage_ = 9;
    smoke_expected_persistence_error_ = ErrorCode::Cancelled;
    smoke_check("pending_text_load_admitted",
                start_persistence(desktop::PersistenceKind::Load, smoke_ + "/typing.scene.json"));
    smoke_type("-");
    smoke_pending_text_ = fields_[0][0]->text();
  }
  if (persistence_smoke_stage_ == 10) {
    smoke_check("load_preserves_intermediate_input", fields_[0][0]->text() == smoke_pending_text_ &&
                                                         !fields_[0][0]->hasAcceptableInput() &&
                                                         document_->generation() == smoke_generation_);
    const auto before = smoke_checks_.size();
    const bool admitted = start_persistence(desktop::PersistenceKind::Save, smoke_ + "/invalid.scene.json");
    const bool one_error =
        smoke_checks_.size() == before + 1 && smoke_checks_.last().toObject().contains("error");
    if (one_error) {
      const auto expected = smoke_checks_.takeAt(before).toObject();
      smoke_checks_.append(QJsonObject{{"check", "intermediate_input_error"},
                                       {"passed", true},
                                       {"expected_error", expected.value("error")}});
    }
    smoke_check("intermediate_input_rejects_save_without_normalization",
                !admitted && one_error && !persistence_->busy() &&
                    fields_[0][0]->text() == smoke_pending_text_);
    smoke_type("7.2500");
    persistence_smoke_stage_ = 11;
    smoke_check("save_admission_commits_pending_text",
                start_persistence(desktop::PersistenceKind::Save, smoke_ + "/admission.scene.json") &&
                    selected_row() && selected_row()->local.translation.x == 7.25);
  }
}
// Stages 12, 14 and 16: saved bytes include pending input, a load keeps the drag preview, close during a
// save, and a missing-file load.
void Editor::smoke_preview_stages() {
  if (persistence_smoke_stage_ == 12) {
    auto saved = desktop::Document::decode(*services_.files, path_string(smoke_ + "/admission.scene.json"),
                                           *document_->schemas());
    auto current = document_->snapshot();
    auto encoded = saved ? scene::encode_json(saved.value(), *document_->schemas(), {})
                         : Result<std::string>{saved.error()};
    auto expected = current ? scene::encode_json(current.value(), *document_->schemas(), {})
                            : Result<std::string>{current.error()};
    smoke_check("saved_bytes_include_pending_input", encoded && expected &&
                                                         encoded.value() == expected.value() && !dirty_ &&
                                                         !pending_inspector());
    smoke_generation_ = document_->generation();
    persistence_smoke_stage_ = 13;
    smoke_expected_persistence_error_ = ErrorCode::Cancelled;
    smoke_check("preview_load_admitted",
                start_persistence(desktop::PersistenceKind::Load, smoke_ + "/typing.scene.json"));
    viewport_->frame_selected();
    smoke_check("preview_remains_unpublished", viewport_->smoke_drag(false, true) && viewport_->previewing());
  }
  if (persistence_smoke_stage_ == 14) {
    smoke_check("load_preserves_viewport_preview",
                viewport_->previewing() && document_->generation() == smoke_generation_);
    viewport_->cancel();
    persistence_smoke_stage_ = 15;
    smoke_check("close_during_save_admitted",
                start_persistence(desktop::PersistenceKind::Save, smoke_ + "/close.scene.json"));
    close();
    smoke_check("close_retains_live_operation", isVisible() && persistence_->busy());
  }
  if (persistence_smoke_stage_ == 16) {
    persistence_smoke_stage_ = 17;
    smoke_expected_persistence_error_ = ErrorCode::NotFound;
    smoke_generation_ = document_->generation();
    smoke_check("missing_file_load_admitted",
                start_persistence(desktop::PersistenceKind::Load, smoke_ + "/missing.scene.json"));
  }
}
// Stage 18, first part: the failed load, cancelled checkpoint dialogs and intermediate-input rejections.
void Editor::smoke_intermediate_input_rejections() {
  smoke_check("failed_load_preserves_document",
              document_->generation() == smoke_generation_ && !persistence_->busy());
  persistence_smoke_stage_ = 19; // The modal dialog runs the Qt event loop.
  const auto untouched = document_->generation();
  smoke_type("6.5000");
  auto *input = fields_[0][0]->findChild<QLineEdit *>();
  input->setSelection(1, 3);
  const auto valid_text = input->text(), valid_selection = input->selectedText();
  const auto valid_cursor = input->cursorPosition();
  smoke_check("cancelled_checkpoint_dialog_preserves_valid_edit",
              !choose_checkpoint(true) && !persistence_->busy() && document_->generation() == untouched &&
                  input->text() == valid_text && input->selectedText() == valid_selection &&
                  input->cursorPosition() == valid_cursor && pending_inspector());
  refresh_inspector();
  const auto generation = document_->generation();
  smoke_type("-");
  const auto pending = fields_[0][0]->text();
  smoke_rejection("enter_reports_intermediate_input", [this] {
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(fields_[0][0], &enter);
  });
  smoke_check("enter_preserves_intermediate_input",
              fields_[0][0]->text() == pending && document_->generation() == generation);
  smoke_check("cancelled_checkpoint_dialog_preserves_intermediate_edit",
              !choose_checkpoint(true) && !persistence_->busy() && fields_[0][0]->text() == pending &&
                  !fields_[0][0]->hasAcceptableInput() && document_->generation() == generation);
  smoke_rejection("play_reports_intermediate_input", [this] { play_action_->trigger(); });
  smoke_check("play_preserves_intermediate_inspector_input",
              !document_->playing() && document_->generation() == generation &&
                  fields_[0][0]->text() == pending && !fields_[0][0]->hasAcceptableInput());
  const auto selected = selection_;
  const auto count = rows_.size();
  smoke_rejection("add_reports_intermediate_input", [this] { add_action_->trigger(); });
  smoke_check("add_preserves_intermediate_input_and_scene", rows_.size() == count && selection_ == selected &&
                                                                document_->generation() == generation &&
                                                                fields_[0][0]->text() == pending);
  smoke_rejection("selection_reports_intermediate_input", [this] {
    for (int i = 0; i < hierarchy_->topLevelItemCount(); ++i) {
      auto *item = hierarchy_->topLevelItem(i);
      if (item != hierarchy_->currentItem()) {
        hierarchy_->setCurrentItem(item);
        break;
      }
    }
  });
  smoke_check("selection_preserves_intermediate_input",
              selection_ == selected && viewport_->selected == selected &&
                  document_->generation() == generation && fields_[0][0]->text() == pending);
  refresh_inspector();
}
// Stage 18, second part: Play commits valid input, recover and alias rejections, and the checkpoint dialog.
void Editor::smoke_play_commit_and_checkpoint() {
  smoke_type("9.1250");
  play_action_->trigger();
  smoke_check("play_commits_valid_pending_input",
              document_->playing() && selected_row() && selected_row()->local.translation.x == 9.125);
  smoke_rejection("recover_reports_active_play", [this] { recover_snapshot(); });
  smoke_check("recover_does_not_stop_play", document_->playing());
  if (document_->playing())
    toggle_play();
  smoke_check("play_admission_edit_has_undo", bool(document_->undo()));
  refresh();
  smoke_authoring_path_ = current_path_;
  smoke_dirty_ = dirty_;
  smoke_selection_ = selection_;
  smoke_generation_ = document_->generation();
  smoke_rejection("checkpoint_rejects_current_file_alias", [this] {
    start_persistence(desktop::PersistenceKind::Checkpoint, current_path_.toUpper());
  });
  smoke_type("-");
  auto *line = fields_[0][0]->findChild<QLineEdit *>();
  line->setSelection(0, 1);
  smoke_pending_text_ = line->text();
  smoke_selected_text_ = line->selectedText();
  smoke_cursor_ = line->cursorPosition();
  persistence_smoke_stage_ = 21;
  smoke_check("checkpoint_dialog_with_intermediate_text_admitted", choose_checkpoint());
}
// Stages 22 and 24: the checkpoint preserves text, UI state and the drag preview; recover is admitted.
void Editor::smoke_checkpoint_stages() {
  if (persistence_smoke_stage_ == 22) {
    auto *line = fields_[0][0]->findChild<QLineEdit *>();
    smoke_check("checkpoint_preserves_text_cursor_selection",
                line->text() == smoke_pending_text_ && line->selectedText() == smoke_selected_text_ &&
                    line->cursorPosition() == smoke_cursor_ && !fields_[0][0]->hasAcceptableInput());
    smoke_check("checkpoint_preserves_authoring_ui_state",
                current_path_ == smoke_authoring_path_ && dirty_ == smoke_dirty_ &&
                    selection_ == smoke_selection_ && document_->generation() == smoke_generation_);
    auto saved = desktop::Document::decode(*services_.files, path_string(smoke_ + "/recovery.scene.json"),
                                           *document_->schemas());
    auto current = document_->snapshot();
    auto actual = saved ? scene::encode_json(saved.value(), *document_->schemas(), {})
                        : Result<std::string>{saved.error()};
    auto expected = current ? scene::encode_json(current.value(), *document_->schemas(), {})
                            : Result<std::string>{current.error()};
    smoke_check("checkpoint_contains_only_published_authoring",
                actual && expected && actual.value() == expected.value());
    refresh_inspector();
    smoke_check("checkpoint_keeps_undo_history", bool(document_->undo()) && bool(document_->redo()));
    refresh();
    viewport_->frame_selected();
    smoke_check("checkpoint_preview_is_unpublished",
                viewport_->smoke_drag(false, true) && viewport_->previewing());
    smoke_generation_ = document_->generation();
    persistence_smoke_stage_ = 23;
    smoke_check(
        "checkpoint_with_preview_admitted",
        start_persistence(desktop::PersistenceKind::Checkpoint, smoke_ + "/preview-recovery.scene.json"));
  }
  if (persistence_smoke_stage_ == 24) {
    smoke_check("checkpoint_preserves_drag_preview", viewport_->previewing() &&
                                                         document_->generation() == smoke_generation_ &&
                                                         selection_ == smoke_selection_);
    viewport_->cancel();
    persistence_smoke_stage_ = 25;
    smoke_check("recover_admitted",
                start_persistence(desktop::PersistenceKind::Recover, smoke_ + "/recovery.scene.json"));
  }
}
// Stages 26, 28 and 30: the recovered scene, then cancelled and missing recoveries.
void Editor::smoke_recovery_stages() {
  if (persistence_smoke_stage_ == 26) {
    smoke_check("recover_is_unsaved_untitled_scene", current_path_.isEmpty() && dirty_ &&
                                                         !document_->playing() && !selection_.valid() &&
                                                         rows_.size() == 2);
    smoke_check("recover_starts_new_undo_history", !document_->undo() && !document_->redo());
    selection_ = rows_.front().id;
    refresh();
    smoke_generation_ = document_->generation();
    persistence_smoke_stage_ = 27;
    smoke_expected_persistence_error_ = ErrorCode::Cancelled;
    smoke_check("recover_pending_text_admitted",
                start_persistence(desktop::PersistenceKind::Recover, smoke_ + "/recovery.scene.json"));
    smoke_type("-");
    smoke_pending_text_ = fields_[0][0]->text();
  }
  if (persistence_smoke_stage_ == 28) {
    smoke_check("recover_cancellation_preserves_pending_text",
                fields_[0][0]->text() == smoke_pending_text_ &&
                    document_->generation() == smoke_generation_ && dirty_ && current_path_.isEmpty());
    refresh_inspector();
    persistence_smoke_stage_ = 29;
    smoke_expected_persistence_error_ = ErrorCode::NotFound;
    smoke_check("missing_recovery_admitted", start_persistence(desktop::PersistenceKind::Recover,
                                                               smoke_ + "/missing-recovery.scene.json"));
  }
  if (persistence_smoke_stage_ == 30) {
    smoke_check("failed_recovery_preserves_unsaved_scene", document_->generation() == smoke_generation_ &&
                                                               dirty_ && current_path_.isEmpty() &&
                                                               selection_ == rows_.front().id);
    persistence_smoke_stage_ = 20;
  }
}
void Editor::smoke_finish() {
  bool passed = true;
  for (const auto &entry : smoke_checks_) {
    auto o = entry.toObject();
    if (o.contains("error") || !o.value("passed").toBool())
      passed = false;
  }
  QJsonObject receipt{
      {"schema", 1},
      {"application", "RACE Qt editor"},
      {"passed", passed},
      {"checks", smoke_checks_},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"scope", "Box authoring and D3D12 raster preview; not production rendering acceptance"}};
  if (viewport_->renderer()) {
    auto s = viewport_->renderer()->stats();
    receipt["adapter"] = QString::fromStdString(s.adapter);
    receipt["hardware"] = s.hardware;
    receipt["width"] = int(s.width);
    receipt["height"] = int(s.height);
    receipt["gpu_ms"] = s.gpu_milliseconds;
    receipt["gpu_timing_available"] = s.gpu_timing_available;
    receipt["debug_layer"] = s.debug_layer;
    receipt["debug_errors"] = qint64(s.debug_errors);
    receipt["debug_warnings"] = qint64(s.debug_warnings);
    if (s.debug_errors || !s.hardware)
      passed = false;
    receipt["passed"] = passed;
  }
  QSaveFile file(smoke_ + "/editor-receipt.json");
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(receipt).toJson()) < 0 || !file.commit())
    passed = false;
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(passed ? 0 : 2);
}
void Editor::smoke_type(const QString &text) {
  auto *spin = fields_[0][0];
  spin->setFocus();
  auto *line = spin->findChild<QLineEdit *>();
  line->selectAll();
  QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, text);
  QCoreApplication::sendEvent(line, &press);
  QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, text);
  QCoreApplication::sendEvent(line, &release);
}
} // namespace race_editor
