// Project content workflows: browser, reimport, surfaces and mesh updates; authoring has its own file.
#include "editor_window.hpp"

namespace race_editor {
bool Editor::content_can_author() const { return !document_->playing() && !content_operation_busy(); }
void Editor::choose_project_browser() {
  if (file_busy())
    return;
  QFileDialog dialog(this, "Open content project", {}, "RACE projects (*.raceproject)");
  dialog.setFileMode(QFileDialog::ExistingFile);
  if (checkpoint_test_) {
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.selectFile(checkpoint_project_);
    QTimer::singleShot(0, &dialog,
                       [&dialog] { QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection); });
  }
  if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty())
    start_project_browser(dialog.selectedFiles().front());
}
QString Editor::project_path() const {
  if (!project_view_)
    return {};
  return QString::fromStdString(project_view_->source.recipe_root) + "/" +
         QString::fromStdString(project_view_->source.project_leaf);
}
content_project::Kind Editor::selected_content_kind() const {
  const auto asset = content_browser_->selected_asset();
  if (asset && project_view_) {
    auto entry = project_view_->project.find(*asset);
    if (entry)
      return entry.value()->reference.kind;
  }
  return content_project::Kind::Mesh;
}
bool Editor::content_can_update() {
  const auto asset = content_browser_->selected_asset();
  const auto *row = selected_row();
  if (!project_view_ || !asset || !row || assets_path_.isEmpty() || !content_can_author())
    return false;
  if (selected_content_kind() == content_project::Kind::Material)
    return row->visible;
  return selected_content_kind() == content_project::Kind::Mesh && row->mesh && row->mesh->asset == *asset;
}
void Editor::refresh_content_context() {
  if (!content_browser_)
    return;
  auto *row = selected_row();
  content_browser_->set_scene(row ? row->mesh : std::nullopt, meshes_->entries(),
                              row ? row->material : std::nullopt, meshes_->materials(), meshes_->textures());
  content_browser_->set_busy(
      project_browser_.busy(), content_operation_busy(), content_can_update(), content_can_reimport(),
      mesh_reimport_.busy() || surface_reimport_.busy() || surface_checkpoint_.busy() ||
          mesh_checkpoint_.busy() || project_authoring_.busy() || material_assignment_.busy(),
      content_can_author(), content_can_author() && bool(project_view_));
}
bool Editor::content_can_reimport() const {
  return project_view_ && content_browser_->selected_asset() && !assets_path_.isEmpty() && meshes_->files() &&
         content_can_author();
}
const desktop::MeshReimportCheckpoint *Editor::matching_checkpoint() const {
  const auto selected = content_browser_->selected_asset();
  if (!reimport_checkpoint_ || !project_view_ || !selected)
    return nullptr;
  const auto &captured = *reimport_checkpoint_;
  if (captured.published.definition.id != *selected ||
      captured.project->project.revision() != project_view_->project.revision() ||
      captured.project->source.recipe_root != project_view_->source.recipe_root ||
      captured.project->source.project_leaf != project_view_->source.project_leaf ||
      captured.roots.product_root != path_string(assets_path_))
    return nullptr;
  return &captured;
}
void Editor::choose_reimport() {
  if (!content_can_reimport())
    return;
  QDialog dialog(this);
  dialog.setWindowTitle("Import/reimport asset");
  dialog.resize(640, 280);
  auto *layout = desktop::ui::dialog_layout(dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  const auto asset = content_browser_->selected_asset();
  const auto entry = project_view_->project.find(*asset);
  if (!entry) {
    error(entry.error().message);
    return;
  }
  auto *source = new QLineEdit(reimport_source_, &dialog);
  source->setObjectName("ReimportSource");
  auto *checkpoints = new QLineEdit(reimport_checkpoints_, &dialog);
  checkpoints->setObjectName("ReimportCheckpoints");
  if (checkpoint_test_) {
    source->setText(QDir(QFileInfo(checkpoint_project_).absolutePath()).absoluteFilePath("../source"));
    checkpoints->setText(QFileInfo(checkpoint_file_).absolutePath());
  }
  auto *selected = new QLineEdit(
      std::visit([](const auto &recipe) { return QString::fromStdString(recipe.definition.source); },
                 entry.value()->recipe),
      &dialog);
  selected->setReadOnly(true);
  form->addRow("Source file", selected);
  reimport_folder(dialog, form, "Source folder", source);
  auto *products = new QLineEdit(assets_path_, &dialog);
  products->setReadOnly(true);
  products->setObjectName("ReimportProducts");
  form->addRow("Products folder", products);
  reimport_folder(dialog, form, "Checkpoints folder", checkpoints);
  auto *hint = new QLabel(
      "Choose separate source, product and checkpoint folders. Import captures a checkpoint. Use Update selected for meshes or Assign material for materials. Texture checkpoints supply pinned material inputs.",
      &dialog);
  hint->setWordWrap(true);
  hint->setObjectName("Subtle");
  layout->addWidget(hint);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  auto *import_button = buttons->addButton("Import/reimport", QDialogButtonBox::AcceptRole);
  import_button->setObjectName("ReimportAccept");
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(source, &QLineEdit::textChanged, &dialog, [=] {
    import_button->setEnabled(!source->text().trimmed().isEmpty() &&
                              !checkpoints->text().trimmed().isEmpty());
  });
  connect(checkpoints, &QLineEdit::textChanged, &dialog, [=] {
    import_button->setEnabled(!source->text().trimmed().isEmpty() &&
                              !checkpoints->text().trimmed().isEmpty());
  });
  import_button->setEnabled(!source->text().trimmed().isEmpty() && !checkpoints->text().trimmed().isEmpty());
  reimport_dialog_smoke(dialog, source, checkpoints, products, buttons, import_button);
  if (dialog.exec() != QDialog::Accepted)
    return;
  const auto source_path = QDir::cleanPath(QFileInfo(source->text()).absoluteFilePath());
  const auto checkpoint_path = QDir::cleanPath(QFileInfo(checkpoints->text()).absoluteFilePath());
  if (start_reimport(source_path, checkpoint_path)) {
    reimport_source_ = source_path;
    reimport_checkpoints_ = checkpoint_path;
  }
}
// One folder row with a Browse button. The connection captures the caller's dialog, which owns it.
void Editor::reimport_folder(QDialog &dialog, QFormLayout *form, const QString &label, QLineEdit *field) {
  auto *cells = new QHBoxLayout;
  cells->setContentsMargins(0, 0, 0, 0);
  cells->addWidget(field, 1);
  auto *browse = new QAction("Browse", &dialog);
  cells->addWidget(desktop::ui::button(&dialog, browse, false, true));
  connect(browse, &QAction::triggered, &dialog, [&dialog, field, label] {
    auto path = QFileDialog::getExistingDirectory(&dialog, label, field->text());
    if (!path.isEmpty())
      field->setText(path);
  });
  form->addRow(label, cells);
}
// Test hooks: the content and checkpoint smokes check, capture and accept the open dialog. The timers
// capture the caller's dialog, which owns them, and widget pointers by value.
void Editor::reimport_dialog_smoke(QDialog &dialog, QLineEdit *source, QLineEdit *checkpoints,
                                   QLineEdit *products, QDialogButtonBox *buttons,
                                   QPushButton *import_button) {
  if (content_test_)
    QTimer::singleShot(0, &dialog, [this, &dialog, import_button, products, source] {
      smoke_check("content_import_dialog_visible",
                  dialog.isVisible() && import_button->isEnabled() && products->isReadOnly());
      smoke_check("content_import_dialog_capture",
                  dialog.grab().save(smoke_ + "/content-import-" + QString::number(content_stage_) + ".png"));
      checkpoint_stage_ = content_stage_;
      smoke_dialog_curves(dialog, "content_import", source);
      import_button->click();
    });
  if (checkpoint_test_)
    QTimer::singleShot(0, &dialog, [this, &dialog, source, products, import_button, checkpoints, buttons] {
      smoke_check("reimport_dialog_visible", dialog.isVisible() && source->isVisible() &&
                                                 products->isReadOnly() && import_button->isEnabled());
      smoke_check(
          "reimport_dialog_layout",
          source->width() >= 80 && checkpoints->width() >= 80 &&
              dialog.rect().contains(QRect(buttons->mapTo(&dialog, QPoint{}), buttons->size())) &&
              dialog.grab().save(smoke_ + "/reimport-dialog-" + QString::number(checkpoint_stage_) + ".png"));
      smoke_dialog_curves(dialog, "reimport_dialog", source);
      import_button->click();
    });
}
bool Editor::start_reimport(const QString &source, const QString &checkpoints) {
  if (!content_can_reimport())
    return false;
  const auto selected = content_browser_->selected_asset();
  if (selected_content_kind() != content_project::Kind::Mesh) {
    auto started = surface_reimport_.start(
        project_view_, *selected,
        {path_string(source), path_string(assets_path_), path_string(checkpoints), project_epoch_});
    if (!check(started ? Result<void>{} : Result<void>{started.error()}))
      return false;
    surface_import_completion_.reset();
    refresh_asset_controls();
    statusBar()->showMessage("Importing surface and capturing checkpoint...", 10000);
    return true;
  }
  desktop::MeshReimportRequest request{path_string(source), path_string(assets_path_),
                                       path_string(checkpoints), path_string(checkpoints), project_epoch_};
  auto started = mesh_reimport_.start(project_view_, *selected, std::move(request));
  if (!started) {
    error(started.error().message);
    return false;
  }
  reimport_completion_.reset();
  reimport_phase_ = desktop::MeshReimportPhase::Idle;
  reimport_callbacks_ = 0;
  reimport_gap_ms_ = 0;
  reimport_callback_ = Clock::now();
  refresh_asset_controls();
  statusBar()->showMessage("Revalidating mesh project...", 10000);
  return true;
}
void Editor::cancel_reimport() {
  if (surface_reimport_.busy()) {
    auto ignored = surface_reimport_.cancel();
    (void)ignored;
  }
  if (mesh_reimport_.busy()) {
    auto ignored = mesh_reimport_.cancel();
    (void)ignored;
  }
}
void Editor::poll_reimport() {
  if (!mesh_reimport_.busy())
    return;
  const auto progress = mesh_reimport_.progress();
  const auto phase = progress
                         ? (progress.value().cancellation_requested ? desktop::MeshReimportPhase::Stopping
                                                                    : progress.value().phase)
                         : reimport_phase_;
  if (progress && phase != reimport_phase_) {
    reimport_phase_ = phase;
    const std::array<const char *, 9> labels{"Idle",
                                             "Revalidating mesh project...",
                                             "Reading mesh source...",
                                             "Starting mesh importer...",
                                             "Importing mesh...",
                                             "Cooking and verifying mesh products...",
                                             "Capturing immutable checkpoint...",
                                             "Cancelling import; waiting for worker...",
                                             "Mesh import complete"};
    statusBar()->showMessage(labels[static_cast<std::size_t>(reimport_phase_)], 10000);
  }
  auto polled =
      mesh_reimport_.poll(project_view_, content_browser_->selected_asset().value_or(assets::AssetId{}),
                          project_epoch_, reimport_checkpoint_);
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  const auto begin = Clock::now();
  if (reimport_callback_ != Clock::time_point{})
    reimport_gap_ms_ = std::max(reimport_gap_ms_, milliseconds(begin - reimport_callback_));
  reimport_callback_ = {};
  if (reimport_expected_error_) {
    smoke_check("reimport_expected_error",
                !completion.result && completion.result.error().code == *reimport_expected_error_);
    reimport_expected_error_.reset();
  } else if (!completion.result) {
    auto message =
        QString::fromStdString(completion.result.error().message) + ". Previous checkpoint retained.";
    if (!completion.staging_directory.empty())
      message += " Staging: " + QString::fromStdString(completion.staging_directory);
    statusBar()->showMessage(message, 15000);
  }
  if (completion.result && completion.changed) {
    const auto &checkpoint = *reimport_checkpoint_;
    const auto path = QString::fromStdString(checkpoint.roots.checkpoint_root) + "/" +
                      QString::fromStdString(checkpoint.published.leaf);
    content_browser_->set_checkpoint(checkpoint.published, path);
    statusBar()->showMessage("Checkpoint captured. Use Update selected to apply it to the Scene.", 10000);
  }
  refresh_asset_controls();
  if (checkpoint_test_)
    reimport_metrics_.append(
        QJsonObject{{"admission_ms", completion.admission_ms},
                    {"staging_directory", QString::fromStdString(completion.staging_directory)},
                    {"source_sha256", QString::fromStdString(assets::hex(completion.source))},
                    {"revalidate_ms", completion.revalidate_ms},
                    {"source_ms", completion.source_ms},
                    {"import_ms", completion.import_ms},
                    {"cook_publish_ms", completion.cook_publish_ms},
                    {"capture_ms", completion.capture_ms},
                    {"publication_ms", completion.publication_ms},
                    {"qt_publication_ms", milliseconds(Clock::now() - begin)},
                    {"source_bytes", qint64(completion.source_bytes)},
                    {"product_bytes", qint64(completion.product_bytes)},
                    {"retained_bytes", qint64(completion.retained_bytes)},
                    {"callbacks", qint64(reimport_callbacks_)},
                    {"max_callback_gap_ms", reimport_gap_ms_},
                    {"passed", bool(completion.result)},
                    {"changed", completion.changed}});
  reimport_completion_ = std::move(completion);
}
const desktop::SurfaceReimportCheckpoint *Editor::matching_surface_capture() const {
  const auto asset = content_browser_->selected_asset();
  if (!asset || !surface_capture_ || !project_view_)
    return nullptr;
  const auto &capture = *surface_capture_;
  if (capture.published.definition.id != *asset ||
      capture.project->project.revision() != project_view_->project.revision() ||
      capture.project->source.recipe_root != project_view_->source.recipe_root ||
      capture.project->source.project_leaf != project_view_->source.project_leaf ||
      capture.roots.product_root != path_string(assets_path_))
    return nullptr;
  return &capture;
}
void Editor::poll_surface_import() {
  if (!surface_reimport_.busy())
    return;
  auto polled =
      surface_reimport_.poll(project_view_, content_browser_->selected_asset().value_or(assets::AssetId{}),
                             project_epoch_, surface_capture_);
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  if (completion.result && completion.changed) {
    const auto &c = surface_capture_->published;
    const auto &d = c.definition;
    content_browser_->set_capture(desktop::ui::ContentCapture{d.id, d.index_revision, d.recipe_revision,
                                                              d.source, d.cooked_revision, c.revision},
                                  QString::fromStdString(surface_capture_->roots.checkpoint_root) + "/" +
                                      QString::fromStdString(c.leaf));
    if (d.kind == content_project::Kind::Texture) {
      // Capture already checked this exact retained recipe against the native
      // product. Consult its admitted profile without IO on the Qt owner.
      const auto entry = surface_capture_->project->project.find(d.id);
      const auto *recipe = entry ? std::get_if<surface_recipe::Snapshot>(&entry.value()->recipe) : nullptr;
      const auto info = recipe ? surface_pipeline::profile_info(recipe->definition.profile)
                               : Result<surface_pipeline::ProfileInfo>{
                                     fail(ErrorCode::Invariant, "Captured texture recipe missing")};
      if (info && info.value().encoding == assets::surface::Encoding::SrgbColor) {
        authoring_texture_ = assets::surface::Reference{d.id, d.cooked_revision};
        statusBar()->showMessage(
            "Color texture captured. Its pinned revision is available in material recipe authoring.", 10000);
      } else if (info && info.value().cooked_schema == 3 &&
                 info.value().encoding == assets::surface::Encoding::LinearData) {
        authoring_scalar_texture_ = assets::surface::Reference{d.id, d.cooked_revision};
        statusBar()->showMessage(
            "Data texture checkpoint captured. Available for diffuse reflectance (R) material recipes.",
            10000);
      } else
        statusBar()->showMessage("Data texture checkpoint captured.", 10000);
    } else
      statusBar()->showMessage(
          "Material checkpoint captured. Assign material applies it to the selected Scene object.", 10000);
  } else if (!completion.result)
    statusBar()->showMessage(
        QString::fromStdString(completion.result.error().message) + ". Previous checkpoint retained.", 15000);
  if (content_test_)
    content_metrics_.append(QJsonObject{{"operation", "import"},
                                        {"passed", bool(completion.result)},
                                        {"changed", completion.changed},
                                        {"source_bytes", qint64(completion.source_bytes)},
                                        {"product_bytes", qint64(completion.product_bytes)},
                                        {"retained_bytes", qint64(completion.retained_bytes)},
                                        {"admission_ms", completion.admission_ms},
                                        {"build_ms", completion.build_ms},
                                        {"capture_ms", completion.capture_ms},
                                        {"publication_ms", completion.publication_ms}});
  surface_import_completion_ = std::move(completion);
  refresh_asset_controls();
}
bool Editor::start_surface_adoption(const QString &path) {
  if (!content_can_update() || selected_content_kind() != content_project::Kind::Material ||
      !commit_inspector() || viewport_->previewing())
    return false;
  const QFileInfo file(path);
  auto started = surface_checkpoint_.start(*document_, meshes_, selection_, project_view_,
                                           {path_string(file.absolutePath()), path_string(file.fileName()),
                                            *content_browser_->selected_asset(), project_epoch_});
  if (!started) {
    error(started.error().message);
    return false;
  }
  surface_adoption_completion_.reset();
  refresh_asset_controls();
  statusBar()->showMessage("Preparing captured material and texture...", 10000);
  return true;
}
void Editor::poll_surface_adoption() {
  if (!surface_checkpoint_.busy())
    return;
  auto polled = surface_checkpoint_.poll(*document_, meshes_, selection_, project_view_,
                                         content_browser_->selected_asset().value_or(assets::AssetId{}),
                                         project_epoch_, !pending_inspector() && !viewport_->previewing());
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  if (completion.result && completion.changed) {
    changed();
    statusBar()->showMessage("Captured material assigned; Undo restores the previous material", 10000);
  } else if (!completion.result)
    statusBar()->showMessage(QString::fromStdString(completion.result.error().message), 10000);
  if (content_test_)
    content_metrics_.append(QJsonObject{{"operation", "adopt"},
                                        {"passed", bool(completion.result)},
                                        {"changed", completion.changed},
                                        {"product_bytes", qint64(completion.product_bytes)},
                                        {"texture_bytes", qint64(completion.texture_bytes)},
                                        {"snapshot_ms", completion.snapshot_ms},
                                        {"load_ms", completion.load_ms},
                                        {"prepare_ms", completion.prepare_ms},
                                        {"commit_ms", completion.commit_ms}});
  surface_adoption_completion_ = std::move(completion);
  refresh_asset_controls();
}
bool Editor::start_project_browser(const QString &path) {
  if (file_busy())
    return false;
  if (project_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    error("Project request epochs exhausted");
    return false;
  }
  const QFileInfo file(path);
  auto started = project_browser_.start(
      {path_string(file.absolutePath()), path_string(file.fileName()), project_epoch_ + 1});
  if (!started) {
    error(started.error().message);
    return false;
  }
  ++project_epoch_;
  const bool was_hidden = content_dock_->isHidden();
  content_dock_->show();
  content_dock_->raise();
  if (was_hidden)
    resizeDocks({content_dock_}, {240}, Qt::Vertical);
  refresh_asset_controls();
  statusBar()->showMessage("Reading project and recipes...", 10000);
  return true;
}
void Editor::cancel_project_browser() {
  if (project_browser_.busy()) {
    auto ignored = project_browser_.cancel();
    (void)ignored;
  }
}
void Editor::poll_project_browser() {
  if (!project_browser_.busy())
    return;
  auto polled = project_browser_.poll(project_epoch_, project_view_);
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  const auto begin = Clock::now();
  if (completion.result) {
    content_browser_->set_snapshot(
        std::shared_ptr<const content_project::Snapshot>(project_view_, &project_view_->project),
        project_path());
    statusBar()->showMessage("Project snapshot ready", 5000);
  } else if (browser_expected_error_) {
    smoke_check("browser_expected_failure", completion.result.error().code == *browser_expected_error_);
    browser_expected_error_.reset();
  } else
    statusBar()->showMessage(QString::fromStdString(completion.result.error().message) +
                                 ". Previous project view retained.",
                             10000);
  refresh_asset_controls();
  if (checkpoint_test_)
    browser_metrics_.append(QJsonObject{{"admission_ms", completion.admission_ms},
                                        {"read_ms", completion.read_ms},
                                        {"publication_ms", completion.publication_ms},
                                        {"qt_publication_ms", milliseconds(Clock::now() - begin)},
                                        {"entries", qint64(completion.entries)},
                                        {"retained_bytes", qint64(completion.retained_bytes)},
                                        {"passed", bool(completion.result)}});
}
bool Editor::update_from_browser(const QString &checkpoint) {
  if (!content_can_update())
    return false;
  return start_mesh_update(project_path(), checkpoint, project_view_->project.revision());
}
void Editor::cancel_mesh_update() {
  if (surface_checkpoint_.busy()) {
    auto ignored = surface_checkpoint_.cancel();
    (void)ignored;
  }
  if (material_assignment_.busy()) {
    auto ignored = material_assignment_.cancel();
    (void)ignored;
  }
  if (mesh_checkpoint_.busy()) {
    auto ignored = mesh_checkpoint_.cancel();
    (void)ignored;
  }
}
bool Editor::start_mesh_update(const QString &project, const QString &checkpoint,
                               std::optional<assets::Digest> expected_project) {
  if (file_busy() || document_->playing() || !commit_inspector())
    return false;
  if (assets_path_.isEmpty() || !meshes_->files() || !selected_row() || !selected_row()->mesh) {
    error("Select a mesh in an open loose assets folder");
    return false;
  }
  if (viewport_->previewing()) {
    error("Finish or cancel the drag before updating a mesh");
    return false;
  }
  QFileInfo p(project), c(checkpoint);
  desktop::MeshCheckpointRequest request{path_string(p.absolutePath()), path_string(p.fileName()),
                                         path_string(c.absolutePath()), path_string(c.fileName()),
                                         expected_project};
  auto started = mesh_checkpoint_.start(*document_, meshes_, selection_, std::move(request));
  if (!started) {
    error(started.error().message);
    return false;
  }
  checkpoint_completion_.reset();
  checkpoint_callbacks_ = 0;
  checkpoint_gap_ms_ = 0;
  checkpoint_callback_ = Clock::now();
  refresh_asset_controls();
  statusBar()->showMessage("Preparing selected mesh update...", 10000);
  return true;
}
void Editor::poll_mesh_update() {
  if (!mesh_checkpoint_.busy())
    return;
  if (pending_inspector() || viewport_->previewing())
    cancel_mesh_update();
  auto polled = mesh_checkpoint_.poll(*document_, meshes_, selection_);
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  if (checkpoint_callback_ != Clock::time_point{})
    checkpoint_gap_ms_ = std::max(checkpoint_gap_ms_, milliseconds(Clock::now() - checkpoint_callback_));
  checkpoint_callback_ = {};
  if (checkpoint_test_)
    checkpoint_metrics_.append(QJsonObject{{"snapshot_ms", completion.snapshot_ms},
                                           {"load_ms", completion.load_ms},
                                           {"prepare_ms", completion.prepare_ms},
                                           {"commit_ms", completion.commit_ms},
                                           {"product_bytes", qint64(completion.product_bytes)},
                                           {"callbacks", qint64(checkpoint_callbacks_)},
                                           {"max_callback_gap_ms", checkpoint_gap_ms_},
                                           {"changed", completion.changed}});
  if (checkpoint_expected_error_) {
    smoke_check("checkpoint_expected_error_" + QString::number(checkpoint_stage_),
                !completion.result && completion.result.error().code == *checkpoint_expected_error_);
    checkpoint_expected_error_.reset();
  } else if (!completion.result) {
    if (completion.result.error().code == ErrorCode::Cancelled ||
        completion.result.error().code == ErrorCode::StaleHandle)
      statusBar()->showMessage("Mesh update discarded after editing", 5000);
    else
      error(completion.result.error().message);
  }
  if (completion.result && completion.changed) {
    changed();
    statusBar()->showMessage("Selected mesh updated; Undo restores its previous revision", 5000);
  } else if (completion.result)
    statusBar()->showMessage("Selected mesh already uses this revision", 5000);
  checkpoint_completion_ = std::move(completion);
  refresh_asset_controls();
}
} // namespace race_editor
