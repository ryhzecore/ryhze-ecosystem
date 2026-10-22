// Opt-in native content smokes: project authoring, the content browser and reimport, and surface content.
#include "editor_window.hpp"

namespace race_editor {
void Editor::browser_button(const QString &text) {
  smoke_browser_click_ = true;
  const auto click = [&](QWidget *parent) {
    if (!parent)
      return false;
    for (auto *button : parent->findChildren<QToolButton *>())
      if (button->defaultAction() && button->defaultAction()->text() == text && button->isVisible() &&
          button->isEnabled()) {
        button->setFocus(Qt::OtherFocusReason);
        button->click();
        return true;
      }
    return false;
  };
  if (click(content_browser_)) {
    smoke_browser_click_ = false;
    return;
  }
  auto *extension = content_browser_->findChild<QToolButton *>("qt_toolbar_ext_button");
  bool clicked = false;
  if (extension && extension->isVisible() && extension->isEnabled()) {
    QTimer::singleShot(0, this, [&] {
      auto *popup = QApplication::activePopupWidget();
      if (authoring_test_ && popup) {
        const auto pixels = popup->grab();
        const auto image = pixels.toImage();
        smoke_check("content_overflow_rounded_popup",
                    !image.isNull() && image.pixelColor(0, 0).alpha() == 0 &&
                        image.pixelColor(image.width() - 1, 0).alpha() == 0 &&
                        image.pixelColor(0, image.height() - 1).alpha() == 0 &&
                        image.pixelColor(image.width() - 1, image.height() - 1).alpha() == 0);
        smoke_check("content_overflow_capture", pixels.save(smoke_ + "/authoring-overflow-popup.png"));
      }
      clicked = click(popup);
      if (popup)
        popup->close();
    });
    extension->click();
    // Qt may show a nonblocking popup; dispatch its queued visible control.
    if (!clicked)
      QCoreApplication::processEvents();
  }
  smoke_browser_click_ = false;
  smoke_check("browser_overflow_button_" + text, clicked);
}
void Editor::smoke_content_curves() {
  auto *extension = content_browser_->findChild<QToolButton *>("qt_toolbar_ext_button");
  const bool fits = extension && extension->isVisible() && extension->size() == QSize(30, 30) &&
                    extension->parentWidget()->rect().contains(extension->geometry());
  smoke_check("content_overflow_circle_contained", fits);
  if (fits) {
    const auto pixels = extension->grab();
    const auto image = pixels.toImage();
    smoke_check("content_overflow_rounded_pixels",
                !image.isNull() && image.pixelColor(0, 0) == QColor("#111214") &&
                    image.pixelColor(image.width() - 1, 0) == QColor("#111214") &&
                    image.pixelColor(0, image.height() - 1) == QColor("#111214") &&
                    image.pixelColor(image.width() - 1, image.height() - 1) == QColor("#111214") &&
                    image.pixelColor(image.width() / 2, 0) != QColor("#111214"));
    smoke_check("content_overflow_control_capture", pixels.save(smoke_ + "/authoring-overflow-control.png"));
  }
  bool found = false;
  for (auto *tabs : findChildren<QTabBar *>())
    if (tabs->isVisible() && tabs->count() > 1) {
      bool content = false;
      for (int n = 0; n < tabs->count(); ++n)
        content = content || tabs->tabText(n) == "Content";
      if (!content)
        continue;
      found = true;
      const auto rect = tabs->tabRect(tabs->currentIndex());
      const auto pixels = tabs->grab();
      const auto image = pixels.toImage();
      const auto ratio = pixels.devicePixelRatio();
      const auto color = [&](int x, int y) {
        return image.pixelColor(int((x + .5) * ratio), int((y + .5) * ratio));
      };
      smoke_check("content_dock_tabs_rounded_pixels",
                  rect.height() >= 30 && rect.height() <= 36 &&
                      color(rect.left() + 1, rect.top() + 1) == QColor("#111214") &&
                      color(rect.center().x(), rect.top() + 5) == QColor("#165BFA"));
      smoke_check("content_dock_tabs_capture", pixels.save(smoke_ + "/authoring-dock-tabs.png"));
    }
  smoke_check("content_dock_tabs_visible", found);
}
void Editor::authoring_check(const QString &name, bool passed) {
  authoring_checks_.append(QJsonObject{{"check", name}, {"passed", passed}});
  smoke_check("authoring_" + name, passed);
}
// Checkpoint stages 90-93: a cancelled New project dialog, then the empty project and two added recipes,
// each checked once its worker has finished; stage 93 then writes the project fixtures.
void Editor::authoring_smoke_step() {
  if (checkpoint_stage_ == 90) {
    resize(800, 600);
    smoke_type("-");
    smoke_pending_text_ = fields_[0][0]->text();
    authoring_dialog_cancel_ = true;
    authoring_stage_ = 0;
    browser_button("New project");
    authoring_check("cancel_dialog_preserves_state",
                    !project_view_ && !project_authoring_.busy() && authoring_preserved());
    authoring_dialog_cancel_ = false;
    authoring_stage_ = 1;
    checkpoint_stage_ = 91;
    browser_button("New project");
    authoring_check("create_admitted", project_authoring_.busy());
    authoring_check("serializes_operations",
                    !content_can_author() && !content_can_update() && !content_can_reimport() &&
                        !start_persistence(desktop::PersistenceKind::Save, smoke_ + "/busy-authoring.json"));
    return;
  }
  if (checkpoint_stage_ == 91) {
    authoring_check("empty_project_ready", project_view_ && project_view_->project.entries().empty() &&
                                               project_view_->source.project_leaf == "empty.raceproject" &&
                                               authoring_preserved() && content_can_author() &&
                                               !content_can_reimport());
    if (!project_view_) {
      checkpoint_smoke_finish();
      return;
    }
    authoring_stage_ = 2;
    checkpoint_stage_ = 92;
    browser_button("Add recipe");
    authoring_check("first_add_admitted", project_authoring_.busy());
    return;
  }
  if (checkpoint_stage_ == 92) {
    authoring_check("first_recipe_selected",
                    project_view_ && project_view_->project.entries().size() == 1 &&
                        project_view_->source.project_leaf == "one.raceproject" &&
                        content_browser_->selected_asset() == checkpoint_before_.asset &&
                        authoring_preserved());
    if (!project_view_ || project_view_->project.entries().size() != 1) {
      checkpoint_smoke_finish();
      return;
    }
    authoring_check(
        "first_recipe_profile",
        std::get<mesh_recipe::Snapshot>(project_view_->project.entries().front().recipe).definition.profile ==
                (authoring_uv_test_ ? mesh_pipeline::Profile::GeometryUv
                                    : mesh_pipeline::Profile::Geometry) &&
            browser_field("ContentProfile") == (authoring_uv_test_ ? "Geometry + UVs" : "Geometry only"));
    authoring_stage_ = 3;
    checkpoint_stage_ = 93;
    browser_button("Add recipe");
    authoring_check("second_add_admitted", project_authoring_.busy());
    return;
  }
  authoring_smoke_fixtures();
}
// The checkpoint baseline is unchanged, including the pending inspector text.
bool Editor::authoring_preserved() const {
  return document_->generation() == checkpoint_generation_ && !dirty_ && selection_ == checkpoint_selected_ &&
         current_path_ == checkpoint_authoring_path_ && fields_[0][0]->text() == smoke_pending_text_;
}
// Stage 93: the second recipe is selected with the geometry profile in a binary index; then the alias,
// raw-pin and closure-failure project fixtures, the content curves and the narrow layout capture.
void Editor::authoring_smoke_fixtures() {
  authoring_check("second_recipe_selected",
                  project_view_ && project_view_->project.entries().size() == 2 &&
                      project_path() == checkpoint_project_ &&
                      content_browser_->selected_asset() != checkpoint_before_.asset &&
                      authoring_preserved());
  if (!project_view_ || project_view_->project.entries().size() != 2) {
    checkpoint_smoke_finish();
    return;
  }
  authoring_check(
      "second_recipe_geometry_profile",
      std::get<mesh_recipe::Snapshot>(project_view_->project.entries().back().recipe).definition.profile ==
              mesh_pipeline::Profile::Geometry &&
          browser_field("ContentProfile") == "Geometry only");
  authoring_check("binary_project_format",
                  project_view_->project.format() == content_project::Format::Binary);
  // A canonical recipe alias preserves exact recipe contents while changing
  // the binary project revision, exercising the displayed-context pin.
  std::vector<content_project::Reference> references;
  for (const auto &value : project_view_->project.entries())
    references.push_back(value.reference);
  const auto write = [&](const QString &name, const QByteArray &payload) {
    QSaveFile file(QFileInfo(checkpoint_project_).absolutePath() + "/" + name);
    return file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size() && file.commit();
  };
  const auto &alias_bytes =
      std::get<mesh_recipe::Snapshot>(project_view_->project.entries().back().recipe).bytes;
  authoring_check("recipe_alias_fixture", write("alternate-recipe.json",
                                                QByteArray(reinterpret_cast<const char *>(alias_bytes.data()),
                                                           static_cast<qsizetype>(alias_bytes.size()))));
  references.back().recipe = "alternate-recipe.json";
  auto changed = content_project::encode_binary(references);
  authoring_check("binary_fixture_encoding", bool(changed));
  if (!changed) {
    checkpoint_smoke_finish();
    return;
  }
  authoring_check("raw_pin_fixture", write("changed-project.raceproject",
                                           QByteArray(reinterpret_cast<const char *>(changed.value().data()),
                                                      static_cast<qsizetype>(changed.value().size()))));
  references.back().revision.fill(std::byte{255});
  auto bad = content_project::encode_binary(references);
  authoring_check(
      "closure_failure_fixture",
      bad && write("bad-project.raceproject", QByteArray(reinterpret_cast<const char *>(bad.value().data()),
                                                         static_cast<qsizetype>(bad.value().size()))));
  authoring_check("select_original_asset", content_browser_->select_asset(checkpoint_before_.asset));
  smoke_content_curves();
  authoring_check("narrow_layout_capture", grab().save(smoke_ + "/authoring-narrow-layout-only.png"));
  resize(1280, 800);
  refresh_inspector();
  checkpoint_stage_ = 99;
}
QString Editor::browser_field(const char *name) {
  auto *field = content_browser_->findChild<QLineEdit *>(name);
  return field ? field->text() : QString{};
}
void Editor::browser_check_revisions(const QString &phase, int resident_count) {
  const auto *row = selected_row();
  smoke_check("browser_scene_" + phase, row && row->mesh &&
                                            browser_field("ContentSceneAsset") ==
                                                QString::fromStdString(assets::id_text(row->mesh->asset)) &&
                                            browser_field("ContentSceneRevision") ==
                                                QString::fromStdString(assets::hex(row->mesh->revision)));
  auto *resident = content_browser_->findChild<QPlainTextEdit *>("ContentResident");
  smoke_check(
      "browser_residents_" + phase,
      resident && resident->toPlainText().split('\n', Qt::SkipEmptyParts).size() == resident_count &&
          resident->toPlainText().contains(QString::fromStdString(assets::hex(checkpoint_before_.revision))));
}
// Checkpoint stages 99-109, one per idle frame: open, refresh and cancel through the content browser, the
// narrow layout, the replaced-index rejection and the reimport checkpoint.
void Editor::browser_smoke_step() {
  auto *tree = content_browser_->findChild<QListWidget *>("ContentRows");
  if (checkpoint_stage_ < 101)
    browser_smoke_open(tree);
  else if (checkpoint_stage_ < 105)
    browser_smoke_refresh(tree);
  else if (checkpoint_stage_ < 108)
    browser_smoke_reimport(tree);
  else
    browser_smoke_reimport_selection(tree);
}
// One key press and release on the browser's content rows.
void Editor::browser_key(QListWidget *tree, int value) {
  QKeyEvent press(QEvent::KeyPress, value, Qt::NoModifier),
      release(QEvent::KeyRelease, value, Qt::NoModifier);
  QCoreApplication::sendEvent(tree, &press);
  QCoreApplication::sendEvent(tree, &release);
}
// The checkpoint scene, selection, clean state and authoring path are unchanged.
bool Editor::browser_preserved() const {
  return document_->generation() == checkpoint_generation_ && !dirty_ && selection_ == checkpoint_selected_ &&
         current_path_ == checkpoint_authoring_path_;
}
// Stage 99 opens the project over a pending valid edit; stage 100 checks the opened index, its fields, the
// read-only profile and keyboard row navigation, then refreshes over invalid input.
void Editor::browser_smoke_open(QListWidget *tree) {
  if (checkpoint_stage_ == 99) {
    smoke_type("12.5");
    smoke_pending_text_ = fields_[0][0]->text();
    checkpoint_stage_ = 100;
    browser_button("Open project");
    smoke_check("browser_open_control_admits", project_browser_.busy());
    return;
  }
  if (checkpoint_stage_ == 100) {
    smoke_check("browser_complete_project",
                project_view_ && project_view_->project.entries().size() == 2 && tree && tree->count() == 2);
    if (!project_view_ || !tree) {
      checkpoint_smoke_finish();
      return;
    }
    smoke_check("browser_open_preserves_pending_valid_edit",
                browser_preserved() && fields_[0][0]->text() == smoke_pending_text_ && pending_inspector());
    smoke_check("browser_raw_index_hash",
                browser_field("ContentIndexHash") ==
                    QString::fromStdString(assets::hex(project_view_->project.revision())));
    for (const auto *name :
         {"ContentAsset", "ContentSource", "ContentProfile", "ContentRecipe", "ContentRecipeHash"})
      smoke_check(QString("browser_field_") + name, !browser_field(name).isEmpty());
    auto *profile = content_browser_->findChild<QLineEdit *>("ContentProfile");
    smoke_check("browser_profile_read_only",
                profile && profile->isReadOnly() &&
                    profile->text() ==
                        (std::get<mesh_recipe::Snapshot>(project_view_->project.entries().front().recipe)
                                     .definition.profile == mesh_pipeline::Profile::GeometryUv
                             ? "Geometry + UVs"
                             : "Geometry only") &&
                    profile->toolTip() == profile->text());
    browser_check_revisions("initial", 1);
    tree->setFocus(Qt::OtherFocusReason);
    browser_key(tree, Qt::Key_Down);
    smoke_check("browser_keyboard_changes_row_only",
                content_browser_->selected_asset() &&
                    *content_browser_->selected_asset() != checkpoint_before_.asset &&
                    !content_can_update() && browser_preserved());
    browser_key(tree, Qt::Key_Delete);
    browser_key(tree, Qt::Key_Return);
    smoke_check("browser_keyboard_does_not_edit_scene", browser_preserved() && !mesh_checkpoint_.busy());
    browser_key(tree, Qt::Key_Home);
    smoke_check("browser_keyboard_returns_asset", content_can_update());
    smoke_type("-");
    smoke_pending_text_ = fields_[0][0]->text();
    checkpoint_stage_ = 101;
    browser_button("Refresh");
    smoke_check("browser_refresh_control_admits", project_browser_.busy());
    return;
  }
}
// Stages 101-104: refresh keeps invalid input, cancelled and bad-closure refreshes keep the view, then the
// narrow layout and the replaced-index update admission.
void Editor::browser_smoke_refresh(QListWidget *tree) {
  if (checkpoint_stage_ == 101) {
    smoke_check("browser_refresh_preserves_invalid_input", browser_preserved() &&
                                                               fields_[0][0]->text() == smoke_pending_text_ &&
                                                               !fields_[0][0]->hasAcceptableInput());
    checkpoint_stage_ = 102;
    browser_expected_error_ = ErrorCode::Cancelled;
    browser_button("Refresh");
    browser_button("Cancel");
    return;
  }
  if (checkpoint_stage_ == 102) {
    smoke_check("browser_cancel_preserves_view",
                !browser_expected_error_ && browser_preserved() && tree->count() == 2);
    browser_expected_error_ = ErrorCode::InvalidDependency;
    checkpoint_stage_ = 103;
    smoke_check(
        "browser_bad_refresh_admission",
        start_project_browser(QFileInfo(checkpoint_project_).absolutePath() + "/bad-project.raceproject"));
    return;
  }
  if (checkpoint_stage_ == 103) {
    smoke_check("browser_bad_closure_preserves_prior_view",
                !browser_expected_error_ && browser_preserved() && tree->count() == 2 &&
                    project_path() == checkpoint_project_ &&
                    browser_field("ContentIndexHash") ==
                        QString::fromStdString(assets::hex(project_view_->project.revision())));
    resize(800, 600);
    resizeDocks({content_dock_}, {180}, Qt::Vertical);
    checkpoint_stage_ = 104;
    return;
  }
  if (checkpoint_stage_ == 104) {
    smoke_check("browser_narrow_layout", width() == 800 && height() == 600 &&
                                             content_browser_->width() >= 300 && tree->width() >= 100 &&
                                             content_dock_->isVisible());
    for (const auto *name : {"ContentIndexHash", "ContentRecipeHash", "ContentSceneRevision"}) {
      auto *field = content_browser_->findChild<QLineEdit *>(name);
      field->selectAll();
      smoke_check(QString("browser_full_copyable_hash_") + name,
                  field->isReadOnly() && field->text().size() == 64 &&
                      field->selectedText() == field->text() && field->toolTip() == field->text());
    }
    auto *details = content_browser_->findChild<QScrollArea *>("ContentDetailsScroll");
    auto *field = content_browser_->findChild<QLineEdit *>("ContentRecipeHash");
    details->ensureWidgetVisible(field);
    smoke_check("browser_narrow_hash_field",
                field->width() >= 80 && details->horizontalScrollBar()->maximum() == 0);
    smoke_check("browser_narrow_capture", grab().save(smoke_ + "/browser-narrow-layout-only.png"));
    refresh_inspector();
    checkpoint_expected_error_ = ErrorCode::InvalidDependency;
    checkpoint_stage_ = 105;
    smoke_check(
        "browser_digest_pin_admitted",
        start_mesh_update(QFileInfo(checkpoint_project_).absolutePath() + "/changed-project.raceproject",
                          checkpoint_file_, project_view_->project.revision()));
    return;
  }
}
// Stages 105-107: the replaced index is rejected, a cancelled reimport keeps the scene, then the published
// reimport checkpoint, its copyable fields and its per-asset visibility.
void Editor::browser_smoke_reimport(QListWidget *tree) {
  if (checkpoint_stage_ == 105) {
    smoke_check("browser_replaced_index_rejected", !checkpoint_expected_error_ && browser_preserved() &&
                                                       *selected_row()->mesh == checkpoint_before_);
    checkpoint_stage_ = 106;
    reimport_expected_error_ = ErrorCode::Cancelled;
    browser_button("Import/reimport");
    smoke_check("reimport_control_admits", mesh_reimport_.busy());
    browser_button("Cancel");
    return;
  }
  if (checkpoint_stage_ == 106) {
    smoke_check("reimport_cancel_preserves_scene", !reimport_expected_error_ && !reimport_checkpoint_ &&
                                                       browser_preserved() &&
                                                       *selected_row()->mesh == checkpoint_before_);
    smoke_type("-");
    smoke_pending_text_ = fields_[0][0]->text();
    checkpoint_stage_ = 107;
    browser_button("Import/reimport");
    smoke_check("reimport_serializes_content_and_save",
                mesh_reimport_.busy() && !content_can_update() && !content_can_reimport() &&
                    !start_persistence(desktop::PersistenceKind::Save, smoke_ + "/busy-import.json"));
    return;
  }
  if (checkpoint_stage_ == 107) {
    smoke_check("reimport_checkpoint_published", reimport_completion_ && reimport_completion_->result &&
                                                     reimport_completion_->changed && matching_checkpoint());
    if (!matching_checkpoint()) {
      checkpoint_smoke_finish();
      return;
    }
    const auto &captured = *matching_checkpoint();
    smoke_check("reimport_exact_checkpoint_path", browser_field("ContentCheckpoint") == checkpoint_file_ &&
                                                      QFileInfo::exists(checkpoint_file_));
    smoke_check("reimport_does_not_edit_scene",
                browser_preserved() && *selected_row()->mesh == checkpoint_before_ &&
                    fields_[0][0]->text() == smoke_pending_text_ && !fields_[0][0]->hasAcceptableInput());
    smoke_check("reimport_new_product",
                captured.published.definition.cooked_revision != checkpoint_before_.revision);
    for (const auto *name : {"ContentCheckpointHash", "ContentCheckpointRevision", "ContentSourceHash"}) {
      auto *field = content_browser_->findChild<QLineEdit *>(name);
      field->selectAll();
      smoke_check(QString("reimport_copyable_") + name, field->isReadOnly() && field->text().size() == 64 &&
                                                            field->selectedText() == field->text() &&
                                                            field->toolTip() == field->text());
    }
    browser_key(tree, Qt::Key_Down);
    smoke_check("reimport_capture_hidden_for_other_asset",
                browser_field("ContentCheckpoint").isEmpty() && !matching_checkpoint());
    browser_key(tree, Qt::Key_Up);
    smoke_check("reimport_capture_restored_for_asset",
                browser_field("ContentCheckpoint") == checkpoint_file_ && matching_checkpoint());
    auto *details = content_browser_->findChild<QScrollArea *>("ContentDetailsScroll");
    details->ensureWidgetVisible(content_browser_->findChild<QLineEdit *>("ContentCheckpointHash"));
    smoke_check("reimport_layout_capture", grab().save(smoke_ + "/reimport-checkpoint-layout-only.png"));
    refresh_inspector();
    checkpoint_stage_ = 108;
    browser_button("Import/reimport");
    return;
  }
}
// Stages 108-109: an identical reimport capture, then a reimport cancelled across an A-B-A selection keeps
// the checkpoint; checkpoint stage 1 then starts with an update request and a selection A-B-A.
void Editor::browser_smoke_reimport_selection(QListWidget *tree) {
  if (checkpoint_stage_ == 108) {
    smoke_check("reimport_identical_capture",
                reimport_completion_ && reimport_completion_->result && matching_checkpoint() &&
                    browser_field("ContentCheckpoint") == checkpoint_file_ && browser_preserved() &&
                    *selected_row()->mesh == checkpoint_before_);
    checkpoint_stage_ = 109;
    reimport_expected_error_ = ErrorCode::Cancelled;
    browser_button("Import/reimport");
    browser_key(tree, Qt::Key_Down);
    browser_key(tree, Qt::Key_Up);
    return;
  }
  if (checkpoint_stage_ == 109) {
    resize(1280, 800);
    smoke_check("reimport_selection_aba_retains_checkpoint",
                !reimport_expected_error_ && matching_checkpoint() &&
                    browser_field("ContentCheckpoint") == checkpoint_file_ && browser_preserved());
    checkpoint_stage_ = 1;
    checkpoint_expected_error_ = ErrorCode::Cancelled;
    smoke_check("checkpoint_selection_request", update_from_browser(checkpoint_file_));
    smoke_check("checkpoint_selection_aba",
                select_object(checkpoint_other_) && select_object(checkpoint_selected_));
    return;
  }
}
// The surface content smoke: texture, material and scalar texture recipes through the browser actions, one
// stage per frame while no content worker is busy.
void Editor::content_smoke_step() {
  if (closing_smoke_ || frames_ < 3 || content_operation_busy() || !viewport_->renderer())
    return;
  if (!content_ready())
    return;
  if (content_stage_ > 0 &&
      !content_require("content_selected_mesh_retained", selected_row() && selected_row()->mesh))
    return;
  if (content_stage_ < 5)
    content_smoke_texture_stages();
  else if (content_stage_ < 8)
    content_smoke_material_stages();
  else if (content_stage_ < 13)
    content_smoke_scalar_stages();
  else
    content_smoke_scalar_material_stages();
}
// Stop at the first failed precondition. A submitted worker remains owned
// until polling joins it; only then may the failed smoke close its window.
bool Editor::content_ready() {
  if (smoke_passed())
    return true;
  if (!content_operation_busy())
    checkpoint_smoke_finish();
  return false;
}
bool Editor::content_require(const char *name, bool passed) {
  smoke_check(name, passed);
  return content_ready();
}
bool Editor::content_act(const char *name) {
  auto *action = content_browser_->findChild<QAction *>(name);
  if (!content_require("content_action_available", action && action->isEnabled()))
    return false;
  action->trigger();
  return content_ready();
}
// Stages 0-4: a new binary project, a cancelled then added texture recipe, a cancelled import that
// publishes nothing, the texture capture, then the material recipe.
void Editor::content_smoke_texture_stages() {
  if (content_stage_ == 0) {
    if (!content_require("content_mesh_added", add_native_mesh(assets_path_ + "/" + mesh_product_)))
      return;
    mesh_product_.clear();
    content_dock_->show();
    content_dock_->raise();
    if (!content_act("ContentNewProjectAction"))
      return;
    content_stage_ = 1;
    return;
  }
  if (content_stage_ == 1) {
    if (!content_require(
            "content_binary_empty_project",
            project_view_ && project_view_->project.format() == content_project::Format::Binary &&
                project_view_->project.schema() == 2 && project_view_->project.entries().empty()))
      return;
    if (!content_act("ContentAddRecipeAction"))
      return;
    if (!content_require("content_cancel_retains_draft",
                         !project_authoring_.busy() &&
                             authoring_surface_profile_ == surface_recipe::Profile::BmpSrgbMips))
      return;
    if (!content_act("ContentAddRecipeAction"))
      return;
    content_stage_ = 2;
    return;
  }
  if (content_stage_ == 2) {
    if (!content_require(
            "content_texture_preserves_binary_index",
            project_view_ && project_view_->project.format() == content_project::Format::Binary &&
                project_view_->project.schema() == 2 && project_view_->project.entries().size() == 1 &&
                selected_content_kind() == content_project::Kind::Texture))
      return;
    if (!content_act("ContentReimportAction") ||
        !content_require("content_import_worker_admitted", surface_reimport_.busy()) ||
        !content_act("ContentCancelAction"))
      return;
    content_stage_ = 3;
    return;
  }
  if (content_stage_ == 3) {
    if (!content_require("content_cancel_does_not_publish",
                         surface_import_completion_ && !surface_import_completion_->result &&
                             surface_import_completion_->result.error().code == ErrorCode::Cancelled &&
                             !surface_capture_ && !selected_row()->material))
      return;
    if (!content_act("ContentReimportAction"))
      return;
    content_stage_ = 4;
    return;
  }
  if (content_stage_ == 4) {
    if (!content_require("content_texture_capture",
                         surface_import_completion_ && surface_import_completion_->result &&
                             surface_capture_ && authoring_texture_ && !content_can_update()))
      return;
    if (!content_act("ContentAddRecipeAction"))
      return;
    content_stage_ = 5;
    return;
  }
}
// Stages 5-7: a material recipe keeps the texture recipe, its capture leaves the scene unchanged, then its
// adoption, undo/redo and which kinds can be assigned.
void Editor::content_smoke_material_stages() {
  if (content_stage_ == 5) {
    if (!content_require("content_material_keeps_texture_recipe",
                         project_view_ && project_view_->project.entries().size() == 2 &&
                             selected_content_kind() == content_project::Kind::Material))
      return;
    if (!content_act("ContentReimportAction"))
      return;
    content_stage_ = 6;
    return;
  }
  if (content_stage_ == 6) {
    if (!content_require("content_material_capture_without_scene_mutation",
                         surface_import_completion_ && surface_import_completion_->result &&
                             matching_surface_capture() && !selected_row()->material))
      return;
    if (!content_act("ContentUpdateAction") ||
        !content_require("content_adoption_worker_admitted", surface_checkpoint_.busy()))
      return;
    content_stage_ = 7;
    return;
  }
  if (content_stage_ == 7) {
    if (!content_require("content_material_adopted",
                         surface_adoption_completion_ && surface_adoption_completion_->result &&
                             surface_adoption_completion_->changed && selected_row()->material &&
                             meshes_->materials().size() == 1 && meshes_->textures().size() == 1))
      return;
    const auto ref = selected_row()->material;
    if (!content_require("content_material_undo", bool(document_->undo())))
      return;
    refresh();
    if (!content_require("content_undo_clears_material", selected_row() && !selected_row()->material))
      return;
    if (!content_require("content_material_redo", bool(document_->redo())))
      return;
    refresh();
    if (!content_require("content_redo_exact_pin", selected_row() && selected_row()->material == ref))
      return;
    resizeDocks({content_dock_}, {360}, Qt::Vertical);
    if (!content_require("content_browser_material_capture",
                         grab().save(smoke_ + "/content-layout-only.png")))
      return;
    content_browser_->select_asset(assets::AssetId{2, 1});
    if (!content_require("content_texture_cannot_assign", !content_can_update()))
      return;
    content_browser_->select_asset(assets::AssetId{3, 1});
    if (!content_require("content_material_can_assign", content_can_update()))
      return;
    content_stage_ = 8;
    return;
  }
}
// Stages 8-12: PPM and BMP scalar texture recipes, each cancelled, added and imported; their captures keep
// the color pin.
void Editor::content_smoke_scalar_stages() {
  if (content_stage_ == 8 || content_stage_ == 10) {
    if (content_stage_ == 10 &&
        !content_require(
            "content_scalar_ppm_capture",
            surface_import_completion_ && surface_import_completion_->result && matching_surface_capture() &&
                surface_capture_->published.definition.id == assets::AssetId{4, 1} && !content_can_update() &&
                authoring_texture_ && authoring_texture_->asset == assets::AssetId{2, 1}))
      return;
    content_authoring_retry_ = false;
    if (!content_act("ContentAddRecipeAction"))
      return;
    const auto profile =
        content_stage_ == 8 ? surface_recipe::Profile::PpmScalarMips : surface_recipe::Profile::BmpScalarMips;
    if (!content_require("content_scalar_cancel_retains_draft",
                         !project_authoring_.busy() && authoring_surface_profile_ == profile))
      return;
    if (!content_act("ContentAddRecipeAction"))
      return;
    ++content_stage_;
    return;
  }
  if (content_stage_ == 9 || content_stage_ == 11) {
    if (!content_require("content_scalar_typed_index",
                         project_view_ &&
                             project_view_->project.entries().size() ==
                                 std::size_t(content_stage_ == 9 ? 3 : 4) &&
                             selected_content_kind() == content_project::Kind::Texture))
      return;
    if (!content_act("ContentReimportAction"))
      return;
    ++content_stage_;
    return;
  }
  if (content_stage_ == 12) {
    if (!content_require(
            "content_scalar_capture",
            surface_import_completion_ && surface_import_completion_->result && matching_surface_capture() &&
                surface_capture_->published.definition.id == assets::AssetId{5, 1} && !content_can_update()))
      return;
    if (!content_require("content_scalar_keeps_color_pin",
                         authoring_texture_ && authoring_texture_->asset == assets::AssetId{2, 1}))
      return;
    if (!content_require("content_scalar_status",
                         statusBar()->currentMessage().contains("Data texture checkpoint captured")))
      return;
    if (!content_require("content_scalar_browser_capture", grab().save(smoke_ + "/scalar-content.png")))
      return;
    if (!content_act("ContentAddRecipeAction"))
      return;
    content_stage_ = 13;
    return;
  }
}
// Stages 13-15: the scalar material recipe and capture, its adoption as linear-data diffuse reflectance,
// undo/redo, and the restored color meshes.
void Editor::content_smoke_scalar_material_stages() {
  if (content_stage_ == 13) {
    if (!content_require("content_scalar_material_typed_index",
                         project_view_ && project_view_->project.entries().size() == 5 &&
                             selected_content_kind() == content_project::Kind::Material))
      return;
    if (!content_act("ContentReimportAction"))
      return;
    content_stage_ = 14;
    return;
  }
  if (content_stage_ == 14) {
    if (!content_require(
            "content_scalar_material_capture",
            surface_import_completion_ && surface_import_completion_->result && matching_surface_capture() &&
                surface_capture_->published.definition.id == assets::AssetId{6, 1} &&
                selected_row()->material && selected_row()->material->asset == assets::AssetId{3, 1}))
      return;
    content_color_meshes_ = meshes_;
    if (!content_act("ContentUpdateAction"))
      return;
    content_stage_ = 15;
    return;
  }
  if (content_stage_ == 15) {
    const auto material =
        selected_row()->material
            ? meshes_->find_material(*selected_row()->material)
            : Result<const desktop::ResolvedMaterial *>{fail(ErrorCode::NotFound, "Missing material")};
    if (!content_require(
            "content_scalar_material_adopted",
            surface_adoption_completion_ && surface_adoption_completion_->result &&
                surface_adoption_completion_->changed && material &&
                material.value()->material->texture_role() == render::TextureRole::DiffuseReflectanceR &&
                material.value()->material->texture() &&
                material.value()->material->texture()->encoding() == render::TextureEncoding::LinearData))
      return;
    if (!content_require("content_scalar_adopted_capture",
                         grab().save(smoke_ + "/scalar-material-adopted.png")))
      return;
    if (!content_require("content_scalar_material_undo", bool(document_->undo())))
      return;
    refresh();
    if (!content_require("content_scalar_undo_restores_color",
                         selected_row() && selected_row()->material &&
                             selected_row()->material->asset == assets::AssetId{3, 1}))
      return;
    if (!content_require("content_scalar_material_redo", bool(document_->redo())))
      return;
    refresh();
    if (!content_require("content_scalar_redo_restores_pin",
                         selected_row() && selected_row()->material &&
                             selected_row()->material->asset == assets::AssetId{6, 1}))
      return;
    if (!content_require("content_scalar_final_undo", bool(document_->undo())))
      return;
    meshes_ = std::move(content_color_meshes_);
    refresh();
    if (!content_ready())
      return;
    content_test_ = false;
    material_product_.clear();
    material_stage_ = 2;
    content_stage_ = 16;
  }
}
} // namespace race_editor
