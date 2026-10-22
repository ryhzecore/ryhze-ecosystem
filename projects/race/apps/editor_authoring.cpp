// Content project authoring: the New content project / Add content recipe dialog and its worker.
#include "editor_window.hpp"

namespace race_editor {
// The content authoring dialog's widgets and texture lists. choose_project_authoring owns it until the
// dialog closes, so the dialog's connections and test timers can capture it by reference.
struct Editor::AuthoringDialog {
  QDialog &dialog;
  const bool add_recipe;
  QFormLayout *form;
  QLineEdit *recipes{}, *source{}, *products{}, *checkpoints{}, *index{}, *leaf{}, *asset{};
  QComboBox *kind{}, *texture{}, *profile{};
  std::vector<std::pair<assets::surface::Reference, bool>> texture_candidates;
  std::vector<assets::surface::Reference> texture_choices;
  QLabel *issue{};
  QDialogButtonBox *buttons{};
  QPushButton *accept{};
};
void Editor::choose_project_authoring(bool add_recipe) {
  if (!content_can_author() || (add_recipe && !project_view_))
    return;
  QDialog dialog(this);
  dialog.setWindowTitle(add_recipe ? "Add content recipe" : "New content project");
  dialog.resize(700, 460);
  auto *layout = desktop::ui::dialog_layout(dialog);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  AuthoringDialog ui{dialog, add_recipe, form};
  authoring_dialog_fields(ui);
  authoring_texture_candidates(ui);
  ui.profile = new QComboBox(&dialog);
  ui.profile->setObjectName("AuthoringProfile");
  ui.profile->setAccessibleName("Import profile");
  ui.profile->addItem("Geometry only", static_cast<int>(mesh_pipeline::Profile::Geometry));
  ui.profile->addItem("Geometry + UVs", static_cast<int>(mesh_pipeline::Profile::GeometryUv));
  ui.profile->setCurrentIndex(ui.profile->findData(static_cast<int>(authoring_profile_)));
  connect(ui.profile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
          [this, &ui] { refresh_authoring_textures(ui); });
  ui.index->setMaxLength(96);
  ui.leaf->setMaxLength(96);
  ui.asset->setMaxLength(32);
  authoring_dialog_folders(ui);
  if (add_recipe)
    authoring_recipe_rows(ui);
  else {
    ui.leaf->hide();
    ui.asset->hide();
    ui.profile->hide();
    ui.kind->hide();
    ui.texture->hide();
  }
  authoring_restore_draft(ui);
  auto *hint = new QLabel(
      "Choose existing folders and a new .raceproject filename. Previous snapshots remain available. Open the products folder through File > Open assets folder before importing. Use Update selected for meshes or Assign material for materials.",
      &dialog);
  hint->setWordWrap(true);
  hint->setObjectName("Subtle");
  layout->addWidget(hint);
  ui.issue = new QLabel(&dialog);
  ui.issue->setWordWrap(true);
  layout->addWidget(ui.issue);
  ui.buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
  ui.accept =
      ui.buttons->addButton(add_recipe ? "Add recipe" : "Create project", QDialogButtonBox::AcceptRole);
  ui.accept->setObjectName("AuthoringAccept");
  layout->addWidget(ui.buttons);
  connect(ui.buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(ui.buttons, &QDialogButtonBox::accepted, &dialog, [this, &ui] { authoring_accept(ui); });
  if (content_test_)
    QTimer::singleShot(0, &dialog, [this, &ui] { authoring_content_smoke(ui); });
  if (authoring_test_)
    QTimer::singleShot(0, &dialog, [this, &ui] { authoring_dialog_smoke(ui); });
  const auto accepted = dialog.exec() == QDialog::Accepted;
  // Retain draft values on cancellation or a worker failure for an explicit retry.
  authoring_retain_draft(ui);
  if (accepted) {
    content_dock_->show();
    content_dock_->raise();
  }
}
// The folder, file and asset ID fields, then the asset type and pinned texture boxes.
void Editor::authoring_dialog_fields(AuthoringDialog &ui) {
  const auto field = [&](const char *name, const QString &value) {
    auto *edit = new QLineEdit(value, &ui.dialog);
    edit->setObjectName(name);
    return edit;
  };
  ui.recipes =
      field("AuthoringRecipes", ui.add_recipe ? QString::fromStdString(project_view_->source.recipe_root)
                                              : authoring_recipe_root_);
  ui.source = field("AuthoringSourceRoot",
                    authoring_source_root_.isEmpty() ? reimport_source_ : authoring_source_root_);
  ui.products =
      field("AuthoringProducts", authoring_product_root_.isEmpty() ? assets_path_ : authoring_product_root_);
  ui.checkpoints =
      field("AuthoringCheckpoints",
            authoring_checkpoint_root_.isEmpty() ? reimport_checkpoints_ : authoring_checkpoint_root_);
  ui.index =
      field("AuthoringIndex", authoring_index_.isEmpty()
                                  ? "project-" + QUuid::createUuid().toString(QUuid::Id128) + ".raceproject"
                                  : authoring_index_);
  ui.leaf = field("AuthoringSourceLeaf", authoring_source_leaf_);
  ui.asset = field("AuthoringAsset", authoring_asset_.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128)
                                                                : authoring_asset_);
  ui.kind = new QComboBox(&ui.dialog);
  ui.kind->setObjectName("AuthoringKind");
  ui.kind->setAccessibleName("Asset type");
  ui.kind->addItem("Mesh", int(content_project::Kind::Mesh));
  ui.kind->addItem("Texture", int(content_project::Kind::Texture));
  ui.kind->addItem("Material", int(content_project::Kind::Material));
  ui.texture = new QComboBox(&ui.dialog);
  ui.texture->setObjectName("AuthoringTexture");
  ui.texture->setAccessibleName("Pinned texture");
  ui.texture->addItem("No texture");
}
// Pinned texture candidates from the retained captures, the draft and the open project's recipes.
void Editor::authoring_texture_candidates(AuthoringDialog &ui) const {
  // Candidates are pinned by validated captures or recipe declarations. The
  // worker still validates exact cooked bytes before importing or adopting.
  const auto candidate = [&](assets::surface::Reference ref, bool scalar) {
    const auto value = std::pair{ref, scalar};
    if (std::find(ui.texture_candidates.begin(), ui.texture_candidates.end(), value) ==
        ui.texture_candidates.end())
      ui.texture_candidates.push_back(value);
  };
  if (authoring_texture_)
    candidate(*authoring_texture_, false);
  if (authoring_scalar_texture_)
    candidate(*authoring_scalar_texture_, true);
  if (authoring_texture_draft_)
    candidate(*authoring_texture_draft_,
              authoring_surface_profile_ == surface_recipe::Profile::MaterialScalarMips);
  if (project_view_)
    for (const auto &entry : project_view_->project.entries())
      if (const auto *recipe = std::get_if<surface_recipe::Snapshot>(&entry.recipe))
        if (recipe->definition.texture)
          candidate(*recipe->definition.texture,
                    recipe->definition.profile == surface_recipe::Profile::MaterialScalarMips);
}
// Lists the candidates that match the Material profile's scalar or color input, keeping the choice.
void Editor::refresh_authoring_textures(AuthoringDialog &ui) const {
  const auto selected = ui.texture->currentIndex() > 0
                            ? std::optional<assets::surface::Reference>{ui.texture_choices[std::size_t(
                                  ui.texture->currentIndex() - 1)]}
                            : std::nullopt;
  ui.texture->clear();
  ui.texture->addItem("No texture");
  ui.texture_choices.clear();
  const bool material = ui.kind->currentData().toInt() == int(content_project::Kind::Material);
  ui.texture->setEnabled(material);
  if (!material)
    return;
  const bool scalar = ui.profile->currentData().toInt() == int(surface_recipe::Profile::MaterialScalarMips);
  for (const auto &[ref, is_scalar] : ui.texture_candidates)
    if (scalar == is_scalar) {
      ui.texture_choices.push_back(ref);
      ui.texture->addItem(QString::fromStdString(assets::id_text(ref.asset)) + " / " +
                          QString::fromStdString(assets::hex(ref.revision)).left(12));
      ui.texture->setItemData(ui.texture->count() - 1, QString::fromStdString(assets::hex(ref.revision)),
                              Qt::ToolTipRole);
      if (selected == ref)
        ui.texture->setCurrentIndex(ui.texture->count() - 1);
    }
}
// The folder rows and the new project file row.
void Editor::authoring_dialog_folders(AuthoringDialog &ui) {
  const auto folder = [&](const QString &label, QLineEdit *edit, bool locked = false) {
    edit->setMaxLength(4096);
    edit->setReadOnly(locked);
    auto *cells = new QHBoxLayout;
    cells->setContentsMargins(0, 0, 0, 0);
    cells->addWidget(edit, 1);
    if (!locked) {
      auto *browse = new QAction("Browse", &ui.dialog);
      cells->addWidget(desktop::ui::button(&ui.dialog, browse, false, true));
      connect(browse, &QAction::triggered, &ui.dialog, [&ui, edit, label] {
        auto path = QFileDialog::getExistingDirectory(&ui.dialog, label, edit->text());
        if (!path.isEmpty())
          edit->setText(path);
      });
    }
    ui.form->addRow(label, cells);
  };
  folder("Recipes folder", ui.recipes, ui.add_recipe);
  ui.form->addRow("New project file", ui.index);
  folder("Source folder", ui.source);
  folder("Products folder", ui.products);
  folder("Checkpoints folder", ui.checkpoints);
}
// Add recipe only: the source file, asset ID, type, profile and pinned texture rows.
void Editor::authoring_recipe_rows(AuthoringDialog &ui) {
  auto *cells = new QHBoxLayout;
  cells->setContentsMargins(0, 0, 0, 0);
  cells->addWidget(ui.leaf, 1);
  auto *browse = new QAction("Choose source", &ui.dialog);
  cells->addWidget(desktop::ui::button(&ui.dialog, browse, false, true));
  connect(browse, &QAction::triggered, &ui.dialog, [&ui] {
    const auto family = static_cast<content_project::Kind>(ui.kind->currentData().toInt());
    const QString filter = family == content_project::Kind::Mesh      ? "Wavefront OBJ (*.obj)"
                           : family == content_project::Kind::Texture ? "Texture source (*.ppm *.bmp)"
                                                                      : "Material source (*.json)";
    auto path = QFileDialog::getOpenFileName(&ui.dialog, "Choose source", ui.source->text(), filter);
    if (!path.isEmpty()) {
      ui.source->setText(QFileInfo(path).absolutePath());
      ui.leaf->setText(QFileInfo(path).fileName());
    }
  });
  ui.form->addRow("Source file", cells);
  ui.form->addRow("Stable asset ID", ui.asset);
  ui.form->addRow("Asset type", ui.kind);
  ui.form->addRow("Profile", ui.profile);
  ui.form->addRow("Pinned texture", ui.texture);
  ui.texture->setEnabled(false);
  connect(ui.kind, qOverload<int>(&QComboBox::currentIndexChanged), &ui.dialog, [this, &ui] {
    const QSignalBlocker blocked(ui.profile);
    ui.profile->clear();
    const auto family = static_cast<content_project::Kind>(ui.kind->currentData().toInt());
    if (family == content_project::Kind::Mesh) {
      ui.profile->addItem("Geometry only", int(mesh_pipeline::Profile::Geometry));
      ui.profile->addItem("Geometry + UVs", int(mesh_pipeline::Profile::GeometryUv));
    } else
      for (const auto value : {surface_recipe::Profile::PpmSrgb, surface_recipe::Profile::PpmLinear,
                               surface_recipe::Profile::PpmSrgbMips, surface_recipe::Profile::BmpSrgb,
                               surface_recipe::Profile::BmpLinear, surface_recipe::Profile::BmpSrgbMips,
                               surface_recipe::Profile::Material, surface_recipe::Profile::MaterialMips,
                               surface_recipe::Profile::PpmScalarMips, surface_recipe::Profile::BmpScalarMips,
                               surface_recipe::Profile::MaterialScalarMips}) {
        const auto info = surface_pipeline::profile_info(value);
        if (info && info.value().material == (family == content_project::Kind::Material))
          ui.profile->addItem(desktop::ui::surface_profile_label(value), int(value));
      }
    refresh_authoring_textures(ui);
  });
}
// Add recipe only: restores the retained draft's surface profile and pinned texture.
void Editor::authoring_restore_draft(AuthoringDialog &ui) const {
  if (ui.add_recipe && authoring_surface_profile_) {
    const auto info = surface_pipeline::profile_info(*authoring_surface_profile_);
    if (info) {
      ui.kind->setCurrentIndex(info.value().material ? 2 : 1);
      ui.profile->setCurrentIndex(ui.profile->findData(int(*authoring_surface_profile_)));
    }
    if (authoring_texture_draft_)
      for (std::size_t n = 0; n < ui.texture_choices.size(); ++n)
        if (ui.texture_choices[n] == *authoring_texture_draft_)
          ui.texture->setCurrentIndex(int(n) + 1);
  }
}
// Accept: validates the fields and starts the authoring worker; a refusal keeps the dialog open.
void Editor::authoring_accept(AuthoringDialog &ui) {
  desktop::ProjectAuthoringRequest request;
  request.kind = ui.add_recipe ? desktop::ProjectAuthoringKind::Add : desktop::ProjectAuthoringKind::Create;
  const auto root = [](const QString &value) {
    return path_string(QDir::cleanPath(QFileInfo(value).absoluteFilePath()));
  };
  for (auto *edit : {ui.recipes, ui.source, ui.products, ui.checkpoints, ui.index})
    if (edit->text().trimmed().isEmpty()) {
      ui.issue->setText("Choose all folders and a new .raceproject filename.");
      return;
    }
  request.recipe_root = root(ui.recipes->text());
  request.source_root = root(ui.source->text());
  request.product_root = root(ui.products->text());
  request.checkpoint_root = root(ui.checkpoints->text());
  request.new_index_leaf = path_string(ui.index->text());
  if (ui.add_recipe) {
    auto id = assets::parse_id(path_string(ui.asset->text()));
    if (!id) {
      ui.issue->setText(QString::fromStdString(id.error().message));
      return;
    }
    request.asset = id.value();
    request.source_leaf = path_string(ui.leaf->text());
    if (ui.kind->currentData().toInt() == int(content_project::Kind::Mesh))
      request.profile = static_cast<mesh_pipeline::Profile>(ui.profile->currentData().toInt());
    else {
      request.surface_profile = static_cast<surface_recipe::Profile>(ui.profile->currentData().toInt());
      if (ui.texture->isEnabled() && ui.texture->currentIndex() > 0)
        request.texture = ui.texture_choices[static_cast<std::size_t>(ui.texture->currentIndex() - 1)];
    }
  }
  if (project_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    ui.issue->setText("Project request epochs exhausted");
    return;
  }
  request.epoch = project_epoch_ + 1;
  auto started = project_authoring_.start(project_view_, request);
  if (!started) {
    ui.issue->setText(QString::fromStdString(started.error().message));
    return;
  }
  ++project_epoch_;
  refresh_asset_controls();
  statusBar()->showMessage("Writing and verifying project snapshot...", 10000);
  ui.dialog.accept();
}
// Content smoke hook: checks the open dialog's choices, fills it, then retries or accepts it.
void Editor::authoring_content_smoke(AuthoringDialog &ui) {
  const bool scalar = content_stage_ == 8 || content_stage_ == 10;
  const auto requested = content_stage_ == 8    ? surface_recipe::Profile::PpmScalarMips
                         : content_stage_ == 10 ? surface_recipe::Profile::BmpScalarMips
                                                : surface_recipe::Profile::BmpSrgbMips;
  if ((content_stage_ == 1 || scalar) && content_authoring_retry_)
    smoke_check("content_authoring_retry_keeps_profile",
                ui.kind->currentData().toInt() == int(content_project::Kind::Texture) &&
                    ui.profile->currentData().toInt() == int(requested));
  if (content_stage_ == 12) {
    ui.kind->setCurrentIndex(2);
    smoke_check("content_scalar_not_offered_as_color",
                ui.texture_choices.size() == 1 && authoring_texture_ &&
                    ui.texture_choices.front() == *authoring_texture_ && surface_capture_ &&
                    ui.texture_choices.front().asset != surface_capture_->published.definition.id);
    ui.profile->setCurrentIndex(ui.profile->findData(int(surface_recipe::Profile::MaterialScalarMips)));
    smoke_check("content_color_not_offered_as_scalar",
                ui.texture_choices.size() == 1 && authoring_scalar_texture_ &&
                    ui.texture_choices.front() == *authoring_scalar_texture_ &&
                    ui.texture_choices.front().asset == assets::AssetId{5, 1});
    smoke_check("content_scalar_material_choices_capture",
                ui.dialog.grab().save(smoke_ + "/scalar-material-choices.png"));
  }
  authoring_content_smoke_fields(ui, requested);
  smoke_check("content_authoring_dialog_visible",
              ui.dialog.isVisible() && ui.accept->isVisible() && ui.index->width() >= 80);
  smoke_check(
      "content_authoring_dialog_capture",
      ui.dialog.grab().save(smoke_ + "/content-authoring-" + QString::number(content_stage_) + ".png"));
  checkpoint_stage_ = content_stage_;
  smoke_dialog_curves(ui.dialog, "content_authoring", ui.index);
  if (!smoke_passed()) {
    ui.dialog.reject();
    return;
  }
  if ((content_stage_ == 1 || scalar) && !content_authoring_retry_) {
    content_authoring_retry_ = true;
    ui.buttons->button(QDialogButtonBox::Cancel)->click();
  } else {
    ui.accept->click();
    // A refused admission leaves the interactive dialog open. Automated
    // validation must record the refusal instead of waiting for its timeout.
    if (ui.dialog.isVisible()) {
      error("Content authoring admission failed: " + ui.issue->text());
      ui.dialog.reject();
    }
  }
}
// Content smoke hook part: the stage's folders, file, asset, type, profile and texture.
void Editor::authoring_content_smoke_fields(AuthoringDialog &ui, surface_recipe::Profile requested) {
  const auto root = QFileInfo(smoke_).absolutePath();
  ui.recipes->setText(root + "/editor-recipes");
  ui.source->setText(root + "/source");
  ui.products->setText(assets_path_);
  ui.checkpoints->setText(root + "/editor-checkpoints");
  ui.index->setText(!ui.add_recipe         ? "empty.raceproject"
                    : content_stage_ == 12 ? "scalar-material.raceproject"
                    : content_stage_ == 8  ? "scalar-ppm.raceproject"
                    : content_stage_ == 10 ? "scalar-bmp.raceproject"
                    : content_stage_ == 1  ? "texture.raceproject"
                                           : "surfaces.raceproject");
  if (ui.add_recipe) {
    const bool material = content_stage_ == 4 || content_stage_ == 12;
    ui.kind->setCurrentIndex(material ? 2 : 1);
    ui.profile->setCurrentIndex(
        ui.profile->findData(int(content_stage_ == 12 ? surface_recipe::Profile::MaterialScalarMips
                                 : material           ? surface_recipe::Profile::MaterialMips
                                                      : requested)));
    ui.leaf->setText(content_stage_ == 12  ? "scalar-material.json"
                     : material            ? "material.json"
                     : content_stage_ == 8 ? "scalar.ppm"
                                           : "color.bmp");
    ui.asset->setText(content_stage_ == 12   ? "00000000000000060000000000000001"
                      : material             ? "00000000000000030000000000000001"
                      : content_stage_ == 8  ? "00000000000000040000000000000001"
                      : content_stage_ == 10 ? "00000000000000050000000000000001"
                                             : "00000000000000020000000000000001");
    if (material)
      ui.texture->setCurrentIndex(1);
    smoke_check("content_explicit_profile_and_dependency",
                ui.profile->currentIndex() >= 0 &&
                    (!material || (ui.texture->count() > 1 && ui.texture->isEnabled())));
    ui.profile->showPopup();
    smoke_check("content_profile_popup_visible", ui.profile->view()->isVisible());
    ui.profile->hidePopup();
  }
}
// Authoring smoke hook: fills, checks and captures the open dialog, then cancels or accepts it.
void Editor::authoring_dialog_smoke(AuthoringDialog &ui) {
  const auto root = QFileInfo(checkpoint_project_).absolutePath();
  ui.recipes->setText(root);
  ui.source->setText(QDir(root).absoluteFilePath("../source"));
  ui.products->setText(assets_path_);
  ui.checkpoints->setText(QFileInfo(checkpoint_file_).absolutePath());
  ui.index->setText(!ui.add_recipe          ? "empty.raceproject"
                    : authoring_stage_ == 2 ? "one.raceproject"
                                            : "project.raceproject");
  ui.leaf->setText(authoring_stage_ == 2 ? "b.obj" : "unused.obj");
  ui.asset->setText(authoring_stage_ == 2 ? "000000000000004d0000000000000001"
                                          : "000000000000004d0000000000000002");
  if (ui.add_recipe) {
    authoring_check("profile_default_geometry",
                    ui.profile->currentData().toInt() == static_cast<int>(mesh_pipeline::Profile::Geometry) &&
                        ui.profile->currentText() == "Geometry only");
    const bool uv = authoring_uv_test_ && authoring_stage_ == 2;
    if (uv)
      smoke_profile_choices(*ui.profile);
    authoring_check("profile_explicit_selection",
                    ui.profile->count() == 2 &&
                        ui.profile->currentData().toInt() ==
                            static_cast<int>(uv ? mesh_pipeline::Profile::GeometryUv
                                                : mesh_pipeline::Profile::Geometry) &&
                        ui.profile->currentText() == (uv ? "Geometry + UVs" : "Geometry only"));
  }
  authoring_check(
      "dialog_visible",
      ui.dialog.isVisible() && ui.index->isVisible() && ui.accept->isVisible() &&
          (!ui.add_recipe || (ui.leaf->isVisible() && ui.asset->isVisible() && ui.profile->isVisible())));
  authoring_check(
      "dialog_layout",
      ui.index->width() >= 80 &&
          ui.dialog.rect().contains(QRect(ui.buttons->mapTo(&ui.dialog, QPoint{}), ui.buttons->size())) &&
          ui.dialog.grab().save(smoke_ + "/authoring-dialog-" + QString::number(authoring_stage_) + ".png"));
  smoke_dialog_curves(ui.dialog, "authoring_dialog", ui.index);
  if (authoring_dialog_cancel_)
    ui.buttons->button(QDialogButtonBox::Cancel)->click();
  else
    ui.accept->click();
}
// Keeps the dialog's values as the draft for an explicit retry.
void Editor::authoring_retain_draft(AuthoringDialog &ui) {
  authoring_recipe_root_ = ui.recipes->text();
  authoring_source_root_ = ui.source->text();
  authoring_product_root_ = ui.products->text();
  authoring_checkpoint_root_ = ui.checkpoints->text();
  authoring_index_ = ui.index->text();
  authoring_source_leaf_ = ui.leaf->text();
  authoring_asset_ = ui.asset->text();
  if (ui.add_recipe && ui.kind->currentData().toInt() == int(content_project::Kind::Mesh))
    authoring_profile_ = static_cast<mesh_pipeline::Profile>(ui.profile->currentData().toInt());
  if (ui.add_recipe) {
    authoring_surface_profile_ =
        ui.kind->currentData().toInt() == int(content_project::Kind::Mesh)
            ? std::nullopt
            : std::optional<surface_recipe::Profile>{
                  static_cast<surface_recipe::Profile>(ui.profile->currentData().toInt())};
    authoring_texture_draft_ = ui.texture->isEnabled() && ui.texture->currentIndex() > 0
                                   ? std::optional<assets::surface::Reference>{ui.texture_choices[std::size_t(
                                         ui.texture->currentIndex() - 1)]}
                                   : std::nullopt;
  }
}
void Editor::cancel_project_authoring() {
  if (project_authoring_.busy()) {
    auto ignored = project_authoring_.cancel();
    (void)ignored;
  }
}
void Editor::poll_project_authoring() {
  if (!project_authoring_.busy())
    return;
  auto polled = project_authoring_.poll(project_epoch_, project_view_);
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  const auto begin = Clock::now();
  if (completion.result && completion.changed) {
    content_browser_->set_snapshot(
        std::shared_ptr<const content_project::Snapshot>(project_view_, &project_view_->project),
        project_path());
    if (completion.kind == desktop::ProjectAuthoringKind::Add)
      content_browser_->select_asset(completion.asset);
    reimport_source_ = QString::fromStdString(completion.names.source_root);
    reimport_checkpoints_ = QString::fromStdString(completion.names.checkpoint_root);
    authoring_index_.clear();
    authoring_asset_.clear();
    authoring_source_leaf_.clear();
    authoring_profile_ = mesh_pipeline::Profile::Geometry;
    authoring_surface_profile_.reset();
    authoring_texture_draft_.reset();
    const bool products_active = !assets_path_.isEmpty() &&
                                 path_string(assets_path_) == completion.names.product_root &&
                                 meshes_->files();
    QString message = completion.kind == desktop::ProjectAuthoringKind::Create
                          ? "Project created. Add a recipe to begin."
                          : "Recipe added. Use Import/reimport to capture a checkpoint.";
    if (!products_active)
      message += " Open assets folder: " + QString::fromStdString(completion.names.product_root);
    statusBar()->showMessage(message, 15000);
  } else if (!completion.result) {
    const auto message = QString::fromStdString(completion.result.error().message) +
                         ". Previous project view and draft retained.";
    if (content_test_)
      error("Content authoring failed: " + message);
    else
      statusBar()->showMessage(message, 15000);
  }
  refresh_asset_controls();
  if (authoring_test_)
    authoring_metrics_.append(QJsonObject{
        {"kind", completion.kind == desktop::ProjectAuthoringKind::Create ? "create" : "add"},
        {"profile",
         completion.names.profile == mesh_pipeline::Profile::GeometryUv ? "geometry-uv" : "geometry"},
        {"source_sha256", QString::fromStdString(assets::hex(completion.source))},
        {"recipe_leaf", QString::fromStdString(completion.recipe_leaf)},
        {"project_path", project_path()},
        {"index_revision",
         project_view_ ? QString::fromStdString(assets::hex(project_view_->project.revision())) : QString{}},
        {"admission_ms", completion.admission_ms},
        {"revalidate_ms", completion.revalidate_ms},
        {"source_ms", completion.source_ms},
        {"publish_ms", completion.publish_ms},
        {"readback_ms", completion.readback_ms},
        {"publication_ms", completion.publication_ms},
        {"qt_publication_ms", milliseconds(Clock::now() - begin)},
        {"entries", qint64(completion.entries)},
        {"source_bytes", qint64(completion.source_bytes)},
        {"retained_bytes", qint64(completion.retained_bytes)},
        {"passed", bool(completion.result)},
        {"changed", completion.changed}});
}
} // namespace race_editor
