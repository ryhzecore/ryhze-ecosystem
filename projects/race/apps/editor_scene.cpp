// Frame loop and Scene editing: worker polls, Play, assets, materials, inspector and lighting.
#include "editor_window.hpp"

namespace race_editor {
void Editor::frame_tick() {
  const auto callback_start = Clock::now();
  if (startup_)
    startup_->turn();
  poll_workers();
  if (profile_finished_)
    return;
  if (!profile_.isEmpty() && !profile_ready_for_frame())
    return;
  const auto frame_start = file_profile_ ? callback_start : Clock::now();
  const double interval_ms = milliseconds(frame_start - previous_frame_);
  previous_frame_ = frame_start;
  if (!profile_.isEmpty())
    previous_allocations_ = desktop::profile::allocation_snapshot();
  if (!profile_.isEmpty())
    profile_interaction();
  if (profile_failed_) {
    finish_profile(false);
    return;
  }
  const double elapsed = std::min(double(play_clock_.nsecsElapsed()) / 1e9, .05);
  play_clock_.restart();
  viewport_->navigate(elapsed);
  advance_play(elapsed);
  viewport_->simulation_tick = simulation_ticks_;
  const bool rendered = viewport_->render();
  if (startup_)
    desktop::startup::editor_frame(startup_, viewport_, reinterpret_cast<HWND>(winId()), rows_.size(),
                                   rendered);
  if (!profile_.isEmpty() && (!rendered || profile_failed_)) {
    if (!rendered)
      error("Profile frame did not submit a visible render");
    finish_profile(false);
    return;
  }
  ++frames_;
  const auto frame_end = Clock::now();
  if (!profile_.isEmpty())
    profile_sample(interval_ms, milliseconds(frame_end - frame_start));
  update_status_text();
  run_smoke_step();
}
void Editor::poll_workers() {
  poll_project_authoring();
  poll_project_browser();
  poll_reimport();
  poll_surface_import();
  poll_surface_adoption();
  poll_mesh_update();
  poll_material_assignment();
  poll_persistence();
}
void Editor::advance_play(double elapsed) {
  if (document_->playing() && !isMinimized() && isVisible()) {
    play_accumulator_ += profile_.isEmpty() ? elapsed : 1.0 / 60;
    for (int steps = 0; play_accumulator_ >= 1.0 / 60 && steps < 4; ++steps) {
      auto result = document_->tick_play(1.0 / 60);
      play_accumulator_ -= 1.0 / 60;
      if (!result) {
        error(result.error().message);
        check(document_->stop_play());
        play_accumulator_ = 0;
        if (!document_->playing())
          simulation_ticks_ = 0;
        play_clock_.restart();
        refresh_play_controls();
        break;
      }
      ++simulation_ticks_;
      if (!profile_.isEmpty())
        ++profile_play_ticks_;
    }
  } else
    play_accumulator_ = 0;
}
void Editor::update_status_text() {
  if (viewport_->renderer() && frames_ % 30 == 0) {
    const auto &s = viewport_->renderer()->stats();
    status_->setText(QString("%1 objects  |  Scene %2  |  GPU %3 ms")
                         .arg(rows_.size())
                         .arg(document_->generation())
                         .arg(s.gpu_milliseconds, 0, 'f', 2));
    status_->setToolTip(QString::fromStdString(s.adapter) +
                        QString("  |  %1 x %2").arg(s.width).arg(s.height));
  }
}
void Editor::run_smoke_step() {
  if (!smoke_.isEmpty()) {
    if (epoch_test_)
      epoch_smoke_step();
    else if (light_test_)
      light_smoke_step();
    else if (content_test_)
      content_smoke_step();
    else if (ui_test_)
      ui_smoke_step();
    else if (checkpoint_test_)
      checkpoint_smoke_step();
    else if (mesh_test_)
      mesh_smoke_step();
    else
      smoke_step();
  }
}
bool Editor::choose_assets(const QString &path) {
  if (file_busy() || document_->playing())
    return false;
  auto root = platform::make_rooted_files(path_string(path));
  if (!root) {
    error(root.error().message);
    return false;
  }
  std::vector<desktop::MeshReference> refs;
  for (const auto &entry : meshes_->entries())
    refs.push_back(entry.reference);
  auto snapshot = document_->snapshot();
  if (!snapshot) {
    error(snapshot.error().message);
    return false;
  }
  auto current = desktop::mesh_references(snapshot.value());
  if (!current) {
    error(current.error().message);
    return false;
  }
  for (const auto &ref : current.value())
    if (std::find(refs.begin(), refs.end(), ref) == refs.end())
      refs.push_back(ref);
  auto candidate = desktop::MeshSet::prepare(root.value(), refs, {}, retained_materials());
  if (!candidate) {
    error(candidate.error().message);
    return false;
  }
  if (!check(candidate.value()->validate_scene(snapshot.value())))
    return false;
  if (assets_path_ != QFileInfo(path).canonicalFilePath()) {
    surface_capture_.reset();
    authoring_texture_.reset();
    authoring_scalar_texture_.reset();
    content_browser_->set_capture(std::nullopt);
  }
  meshes_ = std::move(candidate).value();
  assets_path_ = QFileInfo(path).canonicalFilePath();
  if (reimport_checkpoint_ && reimport_checkpoint_->roots.product_root != path_string(assets_path_)) {
    reimport_checkpoint_.reset();
    content_browser_->set_checkpoint(std::nullopt);
  }
  refresh_asset_controls();
  statusBar()->showMessage("Assets folder loaded", 5000);
  return true;
}
bool Editor::add_native_mesh(const QString &path) {
  if (file_busy() || document_->playing() || !commit_inspector())
    return false;
  if (!meshes_->files() || QFileInfo(path).canonicalPath().compare(assets_path_, Qt::CaseInsensitive) != 0) {
    error("Select a native mesh directly inside the open assets folder");
    return false;
  }
  const auto leaf = QFileInfo(path).fileName().toUtf8();
  auto ref = desktop::mesh_file_reference(
      *meshes_->files(), std::string_view(leaf.constData(), static_cast<std::size_t>(leaf.size())));
  if (!ref) {
    error(ref.error().message);
    return false;
  }
  std::vector<desktop::MeshReference> refs;
  for (const auto &entry : meshes_->entries())
    refs.push_back(entry.reference);
  refs.push_back(ref.value());
  auto candidate = desktop::MeshSet::prepare(meshes_->files(), refs, meshes_, retained_materials());
  if (!candidate) {
    error(candidate.error().message);
    return false;
  }
  auto snapshot = document_->snapshot();
  if (!snapshot) {
    error(snapshot.error().message);
    return false;
  }
  auto instances = desktop::mesh_references(snapshot.value());
  if (!instances) {
    error(instances.error().message);
    return false;
  }
  instances.value().push_back(ref.value());
  if (!check(candidate.value()->validate_instances(instances.value())))
    return false;
  auto added = document_->add_mesh(ref.value());
  if (!added) {
    error(added.error().message);
    return false;
  }
  meshes_ = std::move(candidate).value();
  viewport_->cancel();
  selection_ = added.value();
  changed();
  viewport_->frame_selected();
  return true;
}
std::vector<desktop::MaterialReference> Editor::retained_materials() const {
  std::vector<desktop::MaterialReference> refs;
  refs.reserve(meshes_->materials().size());
  for (const auto &entry : meshes_->materials())
    refs.push_back(entry.reference);
  return refs;
}
void Editor::choose_material() {
  if (file_busy() || document_->playing() || !commit_inspector())
    return;
  auto *row = selected_row();
  if (!row || !row->visible || assets_path_.isEmpty())
    return;
  QFileDialog dialog(this, "Assign cooked native material", assets_path_, "RACE native material (*.rasset)");
  dialog.setFileMode(QFileDialog::ExistingFile);
  if (!material_product_.isEmpty() && material_stage_ == -1) {
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.selectFile(assets_path_ + "/" + material_product_);
    QTimer::singleShot(0, &dialog,
                       [&dialog] { QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection); });
  }
  if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
    return;
  const QFileInfo path(dialog.selectedFiles().front());
  if (path.canonicalPath().compare(assets_path_, Qt::CaseInsensitive) != 0) {
    error("Select a native material directly inside the open assets folder");
    return;
  }
  // Nested dialogs can dispatch callbacks; re-admit selection and input now.
  if (file_busy() || document_->playing() || !commit_inspector())
    return;
  row = selected_row();
  if (!row)
    return;
  viewport_->cancel();
  auto started = material_assignment_.start(*document_, *row, meshes_, path_string(path.fileName()));
  if (!check(started))
    return;
  refresh_asset_controls();
  statusBar()->showMessage("Preparing material and texture...", 10000);
  if (mesh_test_ && !smoke_.isEmpty()) {
    // No owner poll occurs here: even a finished worker remains retained.
    bool blocked = material_assignment_.busy() && !content_can_author() && !content_can_reimport() &&
                   !content_can_update();
    for (const auto *name : {"ContentNewProjectAction", "ContentOpenAction", "ContentAddRecipeAction",
                             "ContentRefreshAction", "ContentReimportAction", "ContentUpdateAction"}) {
      auto *control = content_browser_->findChild<QAction *>(QString::fromLatin1(name));
      blocked = blocked && control && !control->isEnabled();
    }
    auto *cancel = content_browser_->findChild<QAction *>("ContentCancelAction");
    smoke_check("material_assignment_serializes_content", blocked && cancel && cancel->isEnabled());
  }
}
void Editor::poll_material_assignment() {
  auto result =
      material_assignment_.poll(*document_, selection_, !pending_inspector() && !viewport_->previewing());
  if (!result) {
    error(result.error().message);
    return;
  }
  if (!result.value())
    return;
  auto completion = std::move(*result.value());
  if (completion.result) {
    meshes_ = std::move(completion.prepared);
    changed();
    statusBar()->showMessage("Material assigned", 5000);
  } else if (completion.result.error().code != ErrorCode::Cancelled &&
             completion.result.error().code != ErrorCode::StaleHandle)
    error(completion.result.error().message);
  else
    statusBar()->showMessage(completion.result.error().message, 5000);
  refresh_asset_controls();
}
QAction *Editor::action(QMenu *menu, QString name, QKeySequence shortcut, std::function<void()> callback) {
  auto *a = menu->addAction(name);
  a->setShortcut(shortcut);
  connect(a, &QAction::triggered, this, std::move(callback));
  return a;
}
bool Editor::check(Result<void> result) {
  if (result)
    return true;
  error(result.error().message);
  return false;
}
void Editor::error(QString message) {
  statusBar()->showMessage(message, 10000);
  if (!smoke_.isEmpty()) {
    std::fprintf(stderr, "RACE: %s\n", message.toUtf8().constData());
    smoke_checks_.append(QJsonObject{{"error", message}});
  }
  if (!profile_.isEmpty()) {
    profile_failed_ = true;
    profile_errors_.append(message);
  }
}
desktop::ObjectRow *Editor::selected_row() {
  for (auto &row : rows_)
    if (row.id == selection_)
      return &row;
  return nullptr;
}
void Editor::refresh() {
  auto rows = document_->objects();
  if (!rows) {
    error(rows.error().message);
    return;
  }
  rows_ = std::move(rows).value();
  refreshing_ = true;
  hierarchy_->clear();
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    auto *item =
        new QTreeWidgetItem(hierarchy_, {rows_[i].mesh      ? QString("Mesh %1").arg(rows_[i].id.low)
                                         : rows_[i].visible ? label(rows_[i].id)
                                                            : QString("Object %1").arg(rows_[i].id.low)});
    item->setData(0, Qt::UserRole, qulonglong(i));
    if (rows_[i].id == selection_)
      hierarchy_->setCurrentItem(item);
  }
  refreshing_ = false;
  viewport_->selected = selection_;
  refresh_inspector();
  refresh_lighting_controls();
  update_title();
  refresh_asset_controls();
}
void Editor::refresh_asset_controls() {
  const bool enabled = content_can_author();
  if (assets_action_)
    assets_action_->setEnabled(enabled);
  if (mesh_action_)
    mesh_action_->setEnabled(enabled);
  if (update_mesh_action_)
    update_mesh_action_->setEnabled(enabled && !assets_path_.isEmpty() && selected_row() &&
                                    selected_row()->mesh.has_value());
  if (cancel_mesh_update_action_)
    cancel_mesh_update_action_->setEnabled(mesh_checkpoint_.busy());
  if (material_action_)
    material_action_->setEnabled(enabled && !assets_path_.isEmpty() && selected_row() &&
                                 selected_row()->visible);
  if (clear_material_action_)
    clear_material_action_->setEnabled(enabled && selected_row() && selected_row()->material.has_value());
  if (cancel_material_action_)
    cancel_material_action_->setEnabled(material_assignment_.busy() || surface_checkpoint_.busy());
  refresh_content_context();
}
bool Editor::pending_inspector() const {
  if (!inspector_->isEnabled())
    return false;
  for (std::size_t group = 0; group < fields_.size(); ++group)
    for (std::size_t i = 0; i < 3; ++i)
      if (fields_[group][i]->text() != displayed_text_[group][i])
        return true;
  return false;
}
void Editor::update_title() {
  setWindowTitle("RACE ENGINE | " +
                 (current_path_.isEmpty() ? "Untitled" : QFileInfo(current_path_).fileName()) +
                 ((dirty_ || pending_inspector()) ? " *" : ""));
}
void Editor::refresh_inspector() {
  inspected_generation_ = document_->generation();
  auto *row = selected_row();
  inspector_->setEnabled(row && !document_->playing());
  if (!row) {
    material_revision_->clear();
    asset_id_->clear();
    asset_revision_->clear();
    mesh_details_->hide();
    return;
  }
  mesh_details_->setVisible(row->mesh.has_value());
  refreshing_ = true;
  inspected_values_ = {row->local.translation, angles(row->local.rotation), row->local.scale, row->dimensions,
                       row->color};
  for (std::size_t group = 0; group < inspected_values_.size(); ++group) {
    auto value = inspected_values_[group];
    std::array<double, 3> v{value.x, value.y, value.z};
    for (std::size_t i = 0; i < 3; ++i) {
      fields_[group][i]->setValue(v[i]);
      displayed_[group][i] = fields_[group][i]->value();
      displayed_text_[group][i] = fields_[group][i]->text();
    }
  }
  for (auto *field : fields_[3])
    field->setEnabled(!row->mesh);
  asset_id_->setText(row->mesh ? QString::fromStdString(assets::id_text(row->mesh->asset)) : QString{});
  asset_revision_->setText(row->mesh ? QString::fromStdString(assets::hex(row->mesh->revision)) : QString{});
  material_revision_->setText(row->material ? QString::fromStdString(assets::hex(row->material->revision))
                                            : QString{});
  material_revision_->setCursorPosition(0);
  material_revision_->setToolTip(material_revision_->text());
  asset_id_->setCursorPosition(0);
  asset_revision_->setCursorPosition(0);
  asset_id_->setToolTip(asset_id_->text());
  asset_revision_->setToolTip(asset_revision_->text());
  refreshing_ = false;
}
QScrollArea *Editor::properties_scroll() {
  for (QWidget *w = inspector_ ? inspector_->parentWidget() : nullptr; w; w = w->parentWidget())
    if (auto *s = qobject_cast<QScrollArea *>(w))
      return s;
  return nullptr;
}
void Editor::refresh_lighting_controls() {
  if (!physical_toggle_)
    return;
  auto lighting = document_->lighting();
  if (!lighting) {
    error(lighting.error().message);
    return;
  }
  lighting_ = lighting.value();
  const bool was_refreshing = refreshing_;
  refreshing_ = true;
  const bool physical =
      lighting_.output && lighting_.output->profile == render::LightingProfile::PhysicalDiffuseV1;
  const auto light = lighting_.light.value_or(desktop::DirectionalLightSettings{});
  const auto output = lighting_.output.value_or(desktop::RenderOutputSettings{});
  physical_toggle_->setChecked(physical);
  physical_toggle_->setEnabled(!document_->playing());
  const std::array<double, 10> values{light.illuminance_lux,     output.environment_cd_m2,
                                      output.exposure_stops,     output.reference_luminance_cd_m2,
                                      light.direction.x,         light.direction.y,
                                      light.direction.z,         output.background_cd_m2.x,
                                      output.background_cd_m2.y, output.background_cd_m2.z};
  for (std::size_t i = 0; i < values.size(); ++i) {
    light_fields_[i]->setValue(values[i]);
    light_fields_[i]->setEnabled(physical && !document_->playing());
  }
  output_status_->setText(
      physical
          ? "Output: PhysicalDiffuseV1 - FP16 scene-linear cd/m^2, one exposure, ReinhardRGB-v1, sRGB once, SDR display"
          : "Output: LegacyViewportV1 - unitless Lambert and ambient, SDR display");
  refreshing_ = was_refreshing;
}
void Editor::apply_lighting_controls() {
  if (refreshing_ || !physical_toggle_ || document_->playing() || file_busy())
    return;
  for (auto *spin : light_fields_)
    if (!spin->hasAcceptableInput()) {
      error("Finish the lighting value before continuing");
      spin->setFocus();
      return;
    }
  desktop::LightingSettings requested;
  if (physical_toggle_->isChecked()) {
    auto light = lighting_.light.value_or(desktop::DirectionalLightSettings{});
    auto output = lighting_.output.value_or(desktop::RenderOutputSettings{});
    for (auto *spin : light_fields_)
      spin->interpretText();
    light.enabled = true;
    light.illuminance_lux = light_fields_[0]->value();
    light.direction = {light_fields_[4]->value(), light_fields_[5]->value(), light_fields_[6]->value()};
    output.profile = render::LightingProfile::PhysicalDiffuseV1;
    output.target = render::OutputTarget::DisplaySdr;
    output.environment_cd_m2 = light_fields_[1]->value();
    output.exposure_stops = light_fields_[2]->value();
    output.reference_luminance_cd_m2 = light_fields_[3]->value();
    output.background_cd_m2 = {light_fields_[7]->value(), light_fields_[8]->value(),
                               light_fields_[9]->value()};
    requested.light = light;
    requested.output = output;
  }
  if (requested.light == lighting_.light && requested.output == lighting_.output) {
    refresh_lighting_controls();
    return;
  }
  viewport_->cancel();
  if (!check(document_->set_lighting(requested, document_->generation()))) {
    refresh_lighting_controls();
    return;
  }
  changed();
}
void Editor::apply_inspector(std::size_t group) {
  (void)group;
  if (!refreshing_)
    commit_inspector();
}
bool Editor::commit_inspector() {
  auto *source = selected_row();
  if (!source || !inspector_->isEnabled())
    return true;
  // Validate every pending field before Qt can normalize intermediate input.
  // Commit all groups as one transaction so refreshing one cannot erase another.
  for (const auto &group : fields_)
    for (auto *spin : group)
      if (!spin->hasAcceptableInput()) {
        error("Finish the inspector value before continuing");
        spin->setFocus();
        return false;
      }
  auto row = *source;
  for (std::size_t group = 0; group < fields_.size(); ++group) {
    Vec v = inspected_values_[group];
    bool edited = false;
    for (std::size_t i = 0; i < 3; ++i) {
      auto *spin = fields_[group][i];
      spin->interpretText();
      if (spin->value() != displayed_[group][i]) {
        edited = true;
        const double value = spin->value();
        if (i == 0)
          v.x = value;
        if (i == 1)
          v.y = value;
        if (i == 2)
          v.z = value;
      }
    }
    if (!edited)
      continue;
    if (group == 0)
      row.local.translation = v;
    if (group == 1)
      row.local.rotation = euler(v);
    if (group == 2)
      row.local.scale = v;
    if (group == 3)
      row.dimensions = v;
    if (group == 4)
      row.color = v;
  }
  if (row.local == source->local && row.dimensions == source->dimensions && row.color == source->color) {
    for (std::size_t group = 0; group < fields_.size(); ++group)
      for (std::size_t i = 0; i < 3; ++i)
        displayed_text_[group][i] = fields_[group][i]->text();
    update_title();
    return true;
  }
  viewport_->cancel();
  if (!check(document_->edit(row, inspected_generation_)))
    return false;
  changed();
  return true;
}
void Editor::changed() {
  dirty_ = true;
  refresh();
}
bool Editor::select_object(PersistentId id) {
  if (id == selection_)
    return true;
  cancel_mesh_update();
  if (!commit_inspector()) {
    QSignalBlocker blocked(hierarchy_);
    QTreeWidgetItem *previous = nullptr;
    for (int i = 0; i < hierarchy_->topLevelItemCount(); ++i) {
      auto *item = hierarchy_->topLevelItem(i);
      const auto index = item->data(0, Qt::UserRole).toULongLong();
      if (index < rows_.size() && rows_[index].id == selection_)
        previous = item;
    }
    hierarchy_->setCurrentItem(previous);
    viewport_->selected = selection_;
    return false;
  }
  viewport_->cancel();
  selection_ = id;
  refresh();
  return true;
}
bool Editor::new_scene() {
  cancel_mesh_update();
  auto empty = desktop::MeshSet::prepare(nullptr, {});
  if (!empty) {
    error(empty.error().message);
    return false;
  }
  if (!check(document_->reset()))
    return false;
  meshes_ = std::move(empty).value();
  assets_path_.clear();
  viewport_->cancel();
  current_path_.clear();
  dirty_ = false;
  selection_ = {};
  refresh();
  return true;
}
bool Editor::clear_scene_history() {
  cancel_mesh_update();
  auto snapshot = document_->snapshot();
  if (!snapshot) {
    error(snapshot.error().message);
    return false;
  }
  auto refs = desktop::mesh_references(snapshot.value());
  if (!refs) {
    error(refs.error().message);
    return false;
  }
  auto material_refs = desktop::material_references(snapshot.value());
  if (!material_refs) {
    error(material_refs.error().message);
    return false;
  }
  auto compact = desktop::MeshSet::prepare(meshes_->files(), refs.value(), meshes_, material_refs.value());
  if (!compact) {
    error(compact.error().message);
    return false;
  }
  if (!check(document_->clear_history()))
    return false;
  meshes_ = std::move(compact).value();
  refresh_content_context();
  return true;
}
void Editor::add_box() {
  if (!commit_inspector())
    return;
  viewport_->cancel();
  auto added = document_->add_box();
  if (!added) {
    error(added.error().message);
    return;
  }
  selection_ = added.value();
  changed();
}
void Editor::clear_history() {
  if (document_->playing()) {
    error("Stop Play before clearing undo history");
    return;
  }
  if (!commit_inspector())
    return;
  viewport_->cancel();
  if (QMessageBox::question(
          this, "Clear Undo History",
          "Remove all undo and redo steps? The current Scene is preserved. This cannot be undone.",
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
    return;
  if (clear_scene_history()) {
    refresh_inspector();
    statusBar()->showMessage("Undo and redo history cleared; current Scene preserved", 10000);
  }
}
void Editor::refresh_play_controls() {
  play_action_->setText(document_->playing() ? "Stop" : "Play");
  play_action_->setIcon(
      desktop::ui::icon(document_->playing() ? desktop::ui::Icon::Stop : desktop::ui::Icon::Play));
  add_action_->setEnabled(!document_->playing());
  clear_history_action_->setEnabled(!document_->playing());
  refresh_asset_controls();
  refresh_inspector();
  refresh_lighting_controls(); // Lighting controls freeze while Play owns the Scene and thaw on Stop, with the other Play controls.
}
void Editor::toggle_play() {
  if (file_busy() || !commit_inspector())
    return;
  viewport_->cancel();
  auto result = document_->playing() ? document_->stop_play() : document_->start_play();
  if (!check(result))
    return;
  play_accumulator_ = 0;
  simulation_ticks_ = 0;
  play_clock_.restart();
  refresh_play_controls();
}
} // namespace race_editor
