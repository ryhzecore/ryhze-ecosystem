// Core editor: construction, window parts, frame timer, test heartbeats and teardown.
#include "editor_window.hpp"

namespace race_editor {
Editor::Editor(std::unique_ptr<desktop::Document> doc, platform::Services services, QString shaders,
               QString smoke, QString profile, std::uint64_t profile_frames, Clock::time_point process_start,
               QString file_mode, bool mesh_test, QString mesh_product, QString checkpoint_project,
               QString checkpoint_file, bool ui_test, bool authoring_test, QString material_product,
               bool authoring_uv_test, bool content_test, bool epoch_test, bool light_test,
               desktop::startup::Witness *startup)
    : document_(std::move(doc)), services_(std::move(services)), smoke_(std::move(smoke)),
      shaders_(native_path(shaders)), profile_(std::move(profile)), profile_frames_(profile_frames),
      process_start_(process_start), previous_frame_(Clock::now()), file_profile_(!file_mode.isEmpty()),
      file_async_(file_mode == "async") {
  startup_ = startup;
  epoch_test_ = epoch_test;
  light_test_ = light_test;
  content_test_ = content_test;
  ui_test_ = ui_test;
  authoring_test_ = authoring_test;
  authoring_uv_test_ = authoring_uv_test;
  material_product_ = std::move(material_product);
  mesh_test_ = mesh_test;
  mesh_product_ = std::move(mesh_product);
  checkpoint_project_ = std::move(checkpoint_project);
  checkpoint_file_ = std::move(checkpoint_file);
  checkpoint_test_ = !checkpoint_project_.isEmpty();
  auto empty = desktop::MeshSet::prepare(nullptr, {});
  if (!empty)
    throw std::runtime_error(empty.error().message);
  meshes_ = std::move(empty).value();
  setObjectName("RACEEditor");
  setWindowTitle("RACE ENGINE | Untitled");
  resize(1280, 800);
  setMinimumSize(800, 600);
  auto worker_services = platform::make_services();
  if (!worker_services)
    throw std::runtime_error(worker_services.error().message);
  persistence_ = std::make_unique<desktop::PersistenceTask>(std::move(worker_services.value().files));
  initialize_profile_storage();
  auto *container = build_viewport_panel();
  auto *hierarchy_dock = build_hierarchy_dock();
  build_inspector();
  build_lighting_panel();
  auto *properties = build_properties_dock();
  build_content_dock(hierarchy_dock);
  auto *file = build_file_menu();
  auto *edit = build_edit_menu();
  auto *view = build_view_menu(container, hierarchy_dock, properties);
  auto *header = build_header({file, edit, view});
  build_scene_tools(view);
  resizeDocks({hierarchy_dock}, {150}, Qt::Vertical);
  resizeDocks({properties}, {310}, Qt::Horizontal);
  connect_editor_signals();
  if (smoke_.isEmpty() && profile_.isEmpty()) {
    QSettings settings;
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("layout-workspace-v3").toByteArray());
  }
  header->show();
  refresh();
  if (smoke_.isEmpty() && profile_.isEmpty() && !rows_.empty())
    select_object(rows_.front().id);
  if (!profile_.isEmpty())
    viewport_->frame_all();
  play_clock_.start();
  viewport_->automatic_recovery_enabled = profile_.isEmpty();
  viewport_->on_recovery_boundary = [this] {
    play_accumulator_ = 0;
    play_clock_.restart();
    previous_frame_ = Clock::now();
  };
  if (!profile_.isEmpty())
    qApp->installEventFilter(this);
  previous_allocations_ = desktop::profile::allocation_snapshot();
  start_frame_timer();
  start_test_heartbeats();
}
QWidget *Editor::build_viewport_panel() {
  auto *viewport_panel = new QWidget;
  viewport_panel->setObjectName("Surface");
  auto *viewport_layout = new QVBoxLayout(viewport_panel);
  viewport_layout->setContentsMargins(0, 0, 0, 0);
  viewport_layout->setSpacing(0);
  auto *heading = new QWidget;
  heading->setObjectName("PanelHeader");
  heading->setFixedHeight(42);
  auto *heading_layout = new QHBoxLayout(heading);
  heading_layout->setContentsMargins(12, 5, 12, 5);
  auto *scene_label = new QLabel("Scene");
  scene_label->setObjectName("SectionTitle");
  heading_layout->addWidget(scene_label);
  heading_layout->addStretch();
  auto *preview_label = new QLabel("Raster preview");
  preview_label->setObjectName("Subtle");
  heading_layout->addWidget(preview_label);
  viewport_layout->addWidget(heading);
  viewport_ = new Viewport(*document_, meshes_, shaders_);
  viewport_->set_navigation_enabled(profile_.isEmpty() && !mesh_test_ && !ui_test_);
  auto *container = QWidget::createWindowContainer(viewport_);
  container->setObjectName("SceneViewport");
  desktop::startup::size_editor_viewport(startup_, container);
  container->setFocusPolicy(Qt::StrongFocus);
  container->setAccessibleName(
      "D3D12 Scene viewport. Q selects, W moves, E rotates, R scales. Hold right mouse for WASD flight; Q and E move down and up.");
  viewport_layout->addWidget(container, 1);
  auto *guidance = new QLabel("RMB: look / WASD fly  |  Q/W/E/R: tools  |  F: frame  |  Scroll: zoom");
  guidance->setObjectName("Subtle");
  guidance->setWordWrap(true);
  guidance->setContentsMargins(12, 6, 12, 6);
  viewport_layout->addWidget(guidance);
  setCentralWidget(viewport_panel);
  return container;
}
QDockWidget *Editor::build_hierarchy_dock() {
  // Native dock separators keep Objects below the Scene while Properties
  // spans the right edge, including the bottom Objects area.
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  hierarchy_ = new QTreeWidget;
  hierarchy_->setHeaderHidden(true);
  hierarchy_->setRootIsDecorated(false);
  hierarchy_->setIndentation(0);
  hierarchy_->setMinimumSize(160, 72);
  hierarchy_->setAccessibleName("Scene objects hierarchy");
  auto *hierarchy_dock = new QDockWidget("Objects", this);
  hierarchy_dock->setObjectName("Hierarchy");
  hierarchy_dock->setMinimumHeight(114);
  hierarchy_dock->setWidget(hierarchy_);
  hierarchy_dock->setTitleBarWidget(dock_title(hierarchy_dock));
  addDockWidget(Qt::BottomDockWidgetArea, hierarchy_dock);
  return hierarchy_dock;
}
void Editor::build_inspector() {
  inspector_ = new QWidget;
  inspector_->setObjectName("InspectorBody");
  auto *form = new QFormLayout(inspector_);
  form->setContentsMargins(12, 10, 12, 10);
  form->setHorizontalSpacing(8);
  form->setVerticalSpacing(4);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  mesh_details_ = new QWidget;
  auto *mesh_form = new QFormLayout(mesh_details_);
  mesh_form->setContentsMargins(0, 0, 0, 0);
  asset_id_ = new QLineEdit;
  asset_id_->setReadOnly(true);
  asset_id_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  asset_id_->setAccessibleName("Mesh asset ID");
  mesh_form->addRow("Asset", asset_id_);
  asset_revision_ = new QLineEdit;
  asset_revision_->setReadOnly(true);
  asset_revision_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  asset_revision_->setAccessibleName("Pinned mesh revision");
  mesh_form->addRow("Revision", asset_revision_);
  auto *mesh_help = new QLabel("Selection uses mesh bounds.");
  mesh_help->setWordWrap(true);
  mesh_form->addRow(mesh_help);
  form->addRow(mesh_details_);
  mesh_details_->hide();
  std::array<QString, 5> names{"Position (m)", "Rotation (deg)", "Scale", "Dimensions (m)", "Linear color"};
  for (std::size_t group = 0; group < fields_.size(); ++group) {
    add_inspector_vector(form, names, group);
  }
  add_material_controls(form);
}
void Editor::add_inspector_vector(QFormLayout *form, const std::array<QString, 5> &names, std::size_t group) {
  if (group == 0 || group == 3 || group == 4) {
    auto *title = new QLabel(group == 0 ? "Transform" : group == 3 ? "Geometry" : "Appearance");
    title->setObjectName("SectionTitle");
    title->setContentsMargins(0, group == 0 ? 0 : 6, 0, 0);
    form->addRow(title);
  }
  auto *label = new QLabel(names[group]);
  label->setObjectName("PropertyLabel");
  form->addRow(label);
  auto *row = new QWidget;
  row->setObjectName("InspectorVector");
  auto *vector_layout = new QHBoxLayout(row);
  vector_layout->setContentsMargins(0, 0, 0, 0);
  vector_layout->setSpacing(6);
  form->addRow(row);
  for (std::size_t i = 0; i < 3; ++i) {
    auto *spin = new InspectorNumber;
    spin->on_invalid_commit = [this] { error("Finish the inspector value before continuing"); };
    spin->setDecimals(group == 4 ? 3 : 4);
    spin->setSingleStep(group == 1 ? 5 : group == 4 ? .05 : .1);
    spin->setKeyboardTracking(false);
    spin->setRange(group == 3   ? .01
                   : group == 4 ? 0
                                : -1000000000000.0,
                   group == 3   ? 1000
                   : group == 4 ? 1
                                : 1000000000000.0);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spin->setMinimumWidth(64);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setFixedHeight(desktop::ui::ShapeTokens::field_height);
    const auto axis_name = QString(group == 4 ? "RGB" : "XYZ")[static_cast<qsizetype>(i)];
    spin->setAccessibleName(names[group] + " " + axis_name);
    fields_[group][i] = spin;
    auto *component = new QWidget;
    component->setObjectName("AxisField");
    component->setFixedHeight(desktop::ui::ShapeTokens::field_height + 2);
    auto *component_layout = new QHBoxLayout(component);
    component_layout->setContentsMargins(4, 0, 1, 0);
    component_layout->setSpacing(3);
    auto *axis_label = new QLabel(axis_name);
    axis_label->setObjectName("AxisLabel");
    axis_label->setProperty("axis", static_cast<int>(i));
    axis_label->setFixedWidth(8);
    axis_label->setBuddy(spin);
    component_layout->addWidget(axis_label);
    component_layout->addWidget(spin, 1);
    vector_layout->addWidget(component, 1);
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
      if (!refreshing_)
        cancel_mesh_update();
    });
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this, group] { apply_inspector(group); });
    connect(spin->findChild<QLineEdit *>(), &QLineEdit::textEdited, this, [this] {
      if (refreshing_)
        return;
      // A user can type without changing World generation. Invalidate any
      // deferred destructive action and any in-flight load before publication.
      after_save_ = {};
      cancel_mesh_update();
      if (persistence_->busy() && desktop::reads_scene(persistence_kind_)) {
        auto ignored = persistence_->cancel_load();
        (void)ignored;
      }
      update_title();
    });
  }
}
void Editor::add_material_controls(QFormLayout *form) {
  auto *material_title = new QLabel("Material");
  material_title->setObjectName("PropertyLabel");
  form->addRow(material_title);
  material_revision_ = new QLineEdit;
  material_revision_->setObjectName("MaterialRevision");
  material_revision_->setReadOnly(true);
  material_revision_->setPlaceholderText("Object color");
  material_revision_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  material_revision_->setAccessibleName("Pinned material revision");
  form->addRow(material_revision_);
  auto *material_buttons = new QWidget;
  auto *material_layout = new QHBoxLayout(material_buttons);
  material_layout->setContentsMargins(0, 0, 0, 0);
  material_layout->setSpacing(6);
  material_action_ = new QAction("Assign material", this);
  connect(material_action_, &QAction::triggered, this, [this] { choose_material(); });
  auto *assign = desktop::ui::button(material_buttons, material_action_, false, true);
  assign->setObjectName("UiAssignMaterial");
  material_layout->addWidget(assign);
  clear_material_action_ = new QAction("Clear", this);
  connect(clear_material_action_, &QAction::triggered, this, [this] {
    if (file_busy() || document_->playing() || !commit_inspector())
      return;
    if (auto *row = selected_row()) {
      viewport_->cancel();
      if (check(document_->assign_material(row->id, std::nullopt)))
        changed();
    }
  });
  auto *clear = desktop::ui::button(material_buttons, clear_material_action_, false, true);
  clear->setObjectName("UiClearMaterial");
  material_layout->addWidget(clear);
  material_layout->addStretch();
  form->addRow(material_buttons);
  auto *help = new QLabel(
      "Select a transform handle in the Scene. Drag to edit; Esc cancels.\nEnter applies a typed value. Arrow keys adjust it.\nPlay runs an isolated copy of the Scene.");
  help->setWordWrap(true);
  help->setObjectName("Subtle");
  form->addRow(help);
}
void Editor::build_lighting_panel() {
  // P03.04 Scene-wide lighting: explicit opt-in physical controls with unit labels and the effective output status.
  lighting_panel_ = new QWidget;
  lighting_panel_->setObjectName("LightingPanel");
  auto *light_form = new QFormLayout(lighting_panel_);
  light_form->setContentsMargins(12, 10, 12, 10);
  light_form->setHorizontalSpacing(8);
  light_form->setVerticalSpacing(4);
  light_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  light_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  auto *lighting_title = new QLabel("Lighting");
  lighting_title->setObjectName("SectionTitle");
  light_form->addRow(lighting_title);
  // The caption stays short: a check box cannot wrap, and the Properties column is 310 px wide;
  // the status line below names the effective profile.
  physical_toggle_ = new QCheckBox("Physical lighting");
  physical_toggle_->setObjectName("UiPhysicalLighting");
  physical_toggle_->setAccessibleName("Physical lighting profile (PhysicalDiffuseV1)");
  physical_toggle_->setToolTip(
      "PhysicalDiffuseV1: lux and cd/m^2 lighting with the declared scene-linear output");
  light_form->addRow(physical_toggle_);
  output_status_ = new QLabel;
  output_status_->setObjectName("Subtle");
  output_status_->setAccessibleName("Effective output status");
  output_status_->setWordWrap(true);
  light_form->addRow(output_status_);
  add_lighting_fields(light_form);
  connect(physical_toggle_, &QCheckBox::toggled, this, [this](bool) {
    if (!refreshing_)
      apply_lighting_controls();
  });
  auto *lighting_help = new QLabel(
      "Opt in to photometric lighting: lux and cd/m^2 shade an FP16 scene-linear target; one exposure, ReinhardRGB-v1 and sRGB produce the SDR display image. Grid, selection and gizmos keep their legacy colours.");
  lighting_help->setWordWrap(true);
  lighting_help->setObjectName("Subtle");
  light_form->addRow(lighting_help);
}
void Editor::add_lighting_fields(QFormLayout *light_form) {
  // Row labels stay short so the form fits the 310 px Properties column without a horizontal
  // scroll range; the accessible names carry the full photometric descriptions.
  const std::array<const char *, 10> light_names{
      "Illuminance (lux)", "Environment (cd/m^2)", "Exposure (stops)", "Reference (cd/m^2)", "Direction X",
      "Direction Y",       "Direction Z",          "Background R",     "Background G",       "Background B"};
  const std::array<const char *, 10> light_descriptions{"Illuminance (lux)",
                                                        "Environment luminance (cd/m^2)",
                                                        "Exposure (stops)",
                                                        "Reference luminance (cd/m^2)",
                                                        "Light direction X",
                                                        "Light direction Y",
                                                        "Light direction Z",
                                                        "Background radiance R (cd/m^2)",
                                                        "Background radiance G (cd/m^2)",
                                                        "Background radiance B (cd/m^2)"};
  const std::array<std::array<double, 4>, 10> light_ranges{{{0, 100000, 50, 1},
                                                            {0, 1000, 1, 2},
                                                            {-20, 20, .5, 2},
                                                            {1, 10000, 10, 1},
                                                            {-1e6, 1e6, .1, 3},
                                                            {-1e6, 1e6, .1, 3},
                                                            {-1e6, 1e6, .1, 3},
                                                            {0, 60000, 1, 2},
                                                            {0, 60000, 1, 2},
                                                            {0, 60000, 1, 2}}};
  for (std::size_t i = 0; i < light_fields_.size(); ++i) {
    auto *spin = new InspectorNumber;
    spin->on_invalid_commit = [this] { error("Finish the lighting value before continuing"); };
    spin->setRange(light_ranges[i][0], light_ranges[i][1]);
    spin->setSingleStep(light_ranges[i][2]);
    spin->setDecimals(int(light_ranges[i][3]));
    spin->setKeyboardTracking(false);
    spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    spin->setMinimumWidth(64);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setFixedHeight(desktop::ui::ShapeTokens::field_height);
    spin->setAccessibleName(light_descriptions[i]);
    spin->setToolTip(light_descriptions[i]);
    spin->setObjectName(QString("UiLight%1").arg(i));
    light_fields_[i] = spin;
    auto *label = new QLabel(light_names[i]);
    label->setObjectName("PropertyLabel");
    light_form->addRow(label, spin);
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this] { apply_lighting_controls(); });
  }
}
QDockWidget *Editor::build_properties_dock() {
  auto *properties_body = new QWidget;
  properties_body->setObjectName("PropertiesBody");
  auto *properties_layout = new QVBoxLayout(properties_body);
  properties_layout->setContentsMargins(0, 0, 0, 0);
  properties_layout->setSpacing(0);
  properties_layout->addWidget(inspector_);
  properties_layout->addWidget(lighting_panel_);
  properties_layout->addStretch();
  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setWidget(properties_body);
  scroll->setMinimumWidth(310);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *properties = new QDockWidget("Properties", this);
  properties->setObjectName("Properties");
  properties->setWidget(scroll);
  properties->setTitleBarWidget(dock_title(properties));
  addDockWidget(Qt::RightDockWidgetArea, properties);
  return properties;
}
void Editor::build_content_dock(QDockWidget *hierarchy_dock) {
  content_browser_ = new desktop::ui::ContentBrowser;
  content_dock_ = new QDockWidget("Content", this);
  content_dock_->setObjectName("ContentBrowser");
  content_dock_->setWidget(content_browser_);
  content_dock_->setTitleBarWidget(dock_title(content_dock_));
  addDockWidget(Qt::BottomDockWidgetArea, content_dock_);
  tabifyDockWidget(hierarchy_dock, content_dock_);
  content_dock_->hide();
  content_browser_->on_new_project = [this] { choose_project_authoring(false); };
  content_browser_->on_add_recipe = [this] { choose_project_authoring(true); };
  content_browser_->on_open = [this] { choose_project_browser(); };
  content_browser_->on_refresh = [this] {
    if (project_view_)
      start_project_browser(project_path());
  };
  content_browser_->on_cancel = [this] {
    cancel_project_authoring();
    cancel_project_browser();
    cancel_reimport();
    cancel_mesh_update();
  };
  content_browser_->on_reimport = [this] { choose_reimport(); };
  content_browser_->on_selection = [this] {
    cancel_project_authoring();
    cancel_reimport();
    cancel_mesh_update();
    refresh_content_context();
  };
  content_browser_->on_update = [this] { update_selected_content(); };
}
void Editor::update_selected_content() {
  if (!content_can_update())
    return;
  if (selected_content_kind() == content_project::Kind::Material) {
    if (auto *captured = matching_surface_capture()) {
      start_surface_adoption(QString::fromStdString(captured->roots.checkpoint_root) + "/" +
                             QString::fromStdString(captured->published.leaf));
      return;
    }
    const auto path = QFileDialog::getOpenFileName(this, "Choose material checkpoint", reimport_checkpoints_,
                                                   "Material checkpoint (*.json)");
    if (!path.isEmpty())
      start_surface_adoption(path);
    return;
  }
  if (auto *captured = matching_checkpoint()) {
    update_from_browser(QString::fromStdString(captured->roots.checkpoint_root) + "/" +
                        QString::fromStdString(captured->published.leaf));
    return;
  }
  QFileDialog dialog(this, "Choose immutable mesh checkpoint", {}, "Mesh checkpoint (*.json)");
  dialog.setFileMode(QFileDialog::ExistingFile);
  if (checkpoint_test_) {
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.selectFile(checkpoint_file_);
    QTimer::singleShot(0, &dialog,
                       [&dialog] { QMetaObject::invokeMethod(&dialog, "accept", Qt::DirectConnection); });
  }
  if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty())
    update_from_browser(dialog.selectedFiles().front());
}
QMenu *Editor::build_file_menu() {
  auto *file = menuBar()->addMenu("&File");
  action(file, "&New", QKeySequence::New, [this] { after_discard([this] { new_scene(); }); });
  action(file, "&Open…", QKeySequence::Open, [this] {
    after_discard([this] {
      auto path = QFileDialog::getOpenFileName(this, "Open Scene", {}, "RACE Scene (*.json *.rscene)");
      if (!path.isEmpty())
        start_persistence(desktop::PersistenceKind::Load, path);
    });
  });
  save_action_ = action(file, "&Save", QKeySequence::Save, [this] { save(false); });
  action(file, "Save &As…", QKeySequence::SaveAs, [this] { save(true); });
  action(file, "Cook .rscene…", {}, [this] {
    if (file_busy())
      return;
    auto path = QFileDialog::getSaveFileName(this, "Cook runtime Scene", {}, "RACE binary Scene (*.rscene)");
    if (!path.isEmpty())
      start_persistence(desktop::PersistenceKind::Cook, path);
  });
  action(file, "Save Recovery Snapshot...", {}, [this] { choose_checkpoint(); });
  action(file, "Recover Snapshot...", {}, [this] { recover_snapshot(); });
  cancel_load_action_ = action(file, "Cancel Scene Load", {}, [this] {
    if (check(persistence_->cancel_load()))
      statusBar()->showMessage("Cancelling Scene load…", 10000);
  });
  cancel_load_action_->setEnabled(false);
  cancel_material_action_ = action(file, "Cancel material assignment", {}, [this] {
    if (material_assignment_.busy())
      check(material_assignment_.cancel());
    if (surface_checkpoint_.busy())
      check(surface_checkpoint_.cancel());
  });
  cancel_material_action_->setEnabled(false);
  action(file, "Open content package...", {}, [this] {
    after_discard([this] {
      auto path = QFileDialog::getOpenFileName(this, "Open content package", {}, "RACE package (*.rpak)");
      if (!path.isEmpty())
        open_package(path);
    });
  });
  assets_action_ = action(file, "Open assets folder...", {}, [this] {
    if (file_busy() || document_->playing())
      return;
    auto path = QFileDialog::getExistingDirectory(this, "Open assets folder", assets_path_);
    if (!path.isEmpty())
      choose_assets(path);
  });
  action(file, "Open content project...", {}, [this] { content_browser_->on_open(); });
  update_mesh_action_ = action(file, "Update selected mesh from checkpoint...", {}, [this] {
    if (file_busy() || document_->playing() || !commit_inspector())
      return;
    auto project =
        QFileDialog::getOpenFileName(this, "Choose mesh project", {}, "RACE projects (*.raceproject)");
    if (project.isEmpty())
      return;
    auto checkpoint = QFileDialog::getOpenFileName(this, "Choose immutable mesh checkpoint", {},
                                                   "Mesh checkpoint (*.json)");
    if (checkpoint.isEmpty())
      return;
    start_mesh_update(project, checkpoint);
  });
  cancel_mesh_update_action_ = action(file, "Cancel mesh update", {}, [this] {
    cancel_mesh_update();
    statusBar()->showMessage("Cancelling mesh update...", 5000);
  });
  cancel_mesh_update_action_->setEnabled(false);
  file->addSeparator();
  action(file, "Exit", QKeySequence::Quit, [this] { close(); });
  return file;
}
QMenu *Editor::build_edit_menu() {
  auto *edit = menuBar()->addMenu("&Edit");
  undo_action_ = action(edit, "Undo", QKeySequence::Undo, [this] {
    if (!commit_inspector())
      return;
    viewport_->cancel();
    if (check(document_->undo()))
      changed();
  });
  redo_action_ = action(edit, "Redo", QKeySequence::Redo, [this] {
    if (!commit_inspector())
      return;
    viewport_->cancel();
    if (check(document_->redo()))
      changed();
  });
  clear_history_action_ = action(edit, "Clear Undo History…", {}, [this] { clear_history(); });
  action(edit, "Remove selected", QKeySequence::Delete, [this] {
    if (content_browser_->isAncestorOf(QApplication::focusWidget()))
      return;
    if (!commit_inspector())
      return;
    auto row = selected_row();
    if (row && check(document_->remove(row->id, row->handle, inspected_generation_))) {
      selection_ = {};
      changed();
    }
  });
  return edit;
}
QMenu *Editor::build_view_menu(QWidget *container, QDockWidget *hierarchy_dock, QDockWidget *properties) {
  auto *view = menuBar()->addMenu("&View");
  view->addAction(hierarchy_dock->toggleViewAction());
  view->addAction(properties->toggleViewAction());
  view->addAction(content_dock_->toggleViewAction());
  action(view, "Focus Scene viewport", QKeySequence("Ctrl+1"), [this, container] {
    if (commit_inspector()) {
      container->setFocus(Qt::ShortcutFocusReason);
      viewport_->requestActivate();
    }
  });
  action(view, "Frame selected", QKeySequence(Qt::Key_F), [this] { viewport_->frame_selected(); });
  return view;
}
QToolBar *Editor::build_header(const std::array<QMenu *, 3> &menus) {
  // Two bounded rows keep the product/navigation left and document actions right.
  // QToolBar retains native overflow access when a narrow window cannot show every tool.
  menuBar()->hide();
  for (auto *menu : menus)
    addActions(menu->actions());
  auto *header = addToolBar("Editor header");
  header->setObjectName("MainHeader");
  header->setMovable(false);
  header->setFloatable(false);
  header->toggleViewAction()->setEnabled(false);
  auto *brand = new QLabel("RACE");
  brand->setObjectName("HeaderBrand");
  brand->setAccessibleName("RACE engine");
  header->addWidget(brand);
  auto *badge = new QLabel("EDITOR");
  badge->setObjectName("Eyebrow");
  header->addWidget(badge);
  for (auto *menu : menus) {
    auto *a = new QAction(menu->title().remove('&'), this);
    auto *b = desktop::ui::button(header, a);
    b->setObjectName("Ui" + a->text());
    b->setShortcut(QKeySequence::mnemonic(menu->title()));
    b->setMenu(menu);
    b->setPopupMode(QToolButton::InstantPopup);
    header->addWidget(b);
  }
  auto *spacer = new QWidget;
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  header->addWidget(spacer);
  undo_action_->setIcon(desktop::ui::icon(desktop::ui::Icon::Undo));
  redo_action_->setIcon(desktop::ui::icon(desktop::ui::Icon::Redo));
  auto *undo_button = desktop::ui::button(header, undo_action_, true);
  undo_button->setObjectName("UiUndo");
  header->addWidget(undo_button);
  auto *redo_button = desktop::ui::button(header, redo_action_, true);
  redo_button->setObjectName("UiRedo");
  header->addWidget(redo_button);
  save_action_->setIcon(desktop::ui::icon(desktop::ui::Icon::Save));
  auto *save_button = desktop::ui::button(header, save_action_);
  save_button->setObjectName("UiSave");
  header->addWidget(save_button);
  play_action_ = new QAction("Play", this);
  play_action_->setIcon(desktop::ui::icon(desktop::ui::Icon::Play));
  connect(play_action_, &QAction::triggered, this, [this] { toggle_play(); });
  auto *play_button = desktop::ui::button(header, play_action_);
  play_button->setObjectName("UiPlay");
  play_button->setProperty("primary", true);
  header->addWidget(play_button);
  return header;
}
void Editor::build_scene_tools(QMenu *view) {
  addToolBarBreak();
  auto *toolbar = addToolBar("Scene tools");
  toolbar->setObjectName("SceneTools");
  toolbar->setMovable(false);
  toolbar->setFloatable(false);
  view->addAction(toolbar->toggleViewAction());
  auto add_tool = [&](QAction *a, const QString &name, bool circular = false) {
    auto *b = desktop::ui::button(toolbar, a, circular, true);
    b->setObjectName(name);
    toolbar->addWidget(b);
    return b;
  };
  add_action_ = new QAction("Add box", this);
  add_action_->setIcon(desktop::ui::icon(desktop::ui::Icon::Plus));
  connect(add_action_, &QAction::triggered, this, [this] { add_box(); });
  add_tool(add_action_, "UiAddBox");
  mesh_action_ = new QAction("Add mesh", this);
  mesh_action_->setIcon(desktop::ui::icon(desktop::ui::Icon::Plus));
  connect(mesh_action_, &QAction::triggered, this, [this] {
    if (file_busy() || document_->playing())
      return;
    if (assets_path_.isEmpty()) {
      error("Open an assets folder before adding a native mesh");
      return;
    }
    auto path = QFileDialog::getOpenFileName(this, "Add cooked native mesh", assets_path_,
                                             "RACE native mesh (*.rasset)");
    if (!path.isEmpty())
      add_native_mesh(path);
  });
  add_tool(mesh_action_, "UiAddMesh");
  toolbar->addSeparator();
  auto *tool_group = new QWidget;
  tool_group->setObjectName("SegmentedTools");
  tool_group->setAccessibleName("Scene editing tools");
  tool_group->setFixedHeight(desktop::ui::ShapeTokens::main_height);
  auto *tool_layout = new QHBoxLayout(tool_group);
  tool_layout->setContentsMargins(2, 2, 2, 2);
  tool_layout->setSpacing(2);
  auto *tools = new QActionGroup(this);
  int tool = -1;
  for (const auto *name : {"Select", "Move", "Rotate", "Scale"}) {
    auto *a = new QAction(name, this);
    a->setCheckable(true);
    a->setChecked(tool == 0);
    tools->addAction(a);
    int value = tool++;
    a->setToolTip(QString(name) + " (" +
                  QString(value == -1  ? "Q"
                          : value == 0 ? "W"
                          : value == 1 ? "E"
                                       : "R") +
                  ")");
    connect(a, &QAction::triggered, this, [this, value] {
      viewport_->cancel();
      viewport_->tool = value;
      viewport_->axis = -1;
    });
    auto *mode = desktop::ui::button(tool_group, a, false, true);
    mode->setObjectName("Ui" + QString(name));
    tool_layout->addWidget(mode);
  }
  toolbar->addWidget(tool_group);
  toolbar->addSeparator();
  auto *coordinates = new QLabel("Local");
  coordinates->setObjectName("Subtle");
  coordinates->setToolTip("Transform along the selected object's local axes");
  toolbar->addWidget(coordinates);
  viewport_->on_tool = [tools](int selected) {
    const auto actions = tools->actions();
    if (selected >= -1 && selected < 3)
      actions[selected + 1]->setChecked(true);
  };
}
void Editor::connect_editor_signals() {
  status_ = new QLabel("Starting D3D12…");
  status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  statusBar()->addWidget(status_, 1);
  connect(hierarchy_, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
            if (refreshing_)
              return;
            PersistentId requested;
            // Capture identity before admission can rebuild/delete the hierarchy items.
            if (item) {
              const auto index = item->data(0, Qt::UserRole).toULongLong();
              if (index < rows_.size())
                requested = rows_[index].id;
            }
            select_object(requested);
          });
  viewport_->on_admit_edit = [this] {
    cancel_mesh_update();
    return commit_inspector();
  };
  viewport_->on_select = [this](PersistentId id) { select_object(id); };
  viewport_->on_edit = [this] { changed(); };
  viewport_->on_error = [this](QString message) { error(message); };
}
void Editor::start_frame_timer() {
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, [this] { frame_tick(); });
  timer->start(16);
}
void Editor::start_test_heartbeats() {
  if (checkpoint_test_) {
    auto *heartbeat = new QTimer(this);
    connect(heartbeat, &QTimer::timeout, this, [this] {
      if (mesh_reimport_.busy()) {
        ++reimport_callbacks_;
        auto now = Clock::now();
        if (reimport_callback_ != Clock::time_point{})
          reimport_gap_ms_ = std::max(reimport_gap_ms_, milliseconds(now - reimport_callback_));
        reimport_callback_ = now;
      }
      if (mesh_checkpoint_.busy()) {
        ++checkpoint_callbacks_;
        auto now = Clock::now();
        if (checkpoint_callback_ != Clock::time_point{})
          checkpoint_gap_ms_ = std::max(checkpoint_gap_ms_, milliseconds(now - checkpoint_callback_));
        checkpoint_callback_ = now;
      }
    });
    heartbeat->start(1);
  }
  if (mesh_test_) {
    auto *heartbeat = new QTimer(this);
    connect(heartbeat, &QTimer::timeout, this, [this] {
      ++package_heartbeats_;
      if (package_smoke_stage_ == 1 && persistence_->busy()) {
        ++package_pending_heartbeats_;
        const auto now = Clock::now();
        if (package_previous_callback_ != Clock::time_point{})
          package_max_callback_gap_ms_ =
              std::max(package_max_callback_gap_ms_, milliseconds(now - package_previous_callback_));
        package_previous_callback_ = now;
      }
    });
    heartbeat->start(1);
  }
}
void Editor::initialize_profile_storage() {
  if (!profile_.isEmpty()) {
    setFixedSize(1280, 800);
    profile_gpu_values_.assign(static_cast<std::size_t>(60 + profile_frames_ + 240 + 1), -1.0);
  }
}
void Editor::startup_adopted() {
  if (startup_)
    startup_->adopted(rows_.size());
}
Editor::~Editor() {
  // Retain worker-owned files/schemas/snapshots until physical I/O completes.
  cancel_project_authoring();
  cancel_project_browser();
  cancel_reimport();
  cancel_mesh_update();
  persistence_.reset();
  if (!profile_.isEmpty())
    qApp->removeEventFilter(this);
  // Quiesce the renderer before Qt begins destroying its native surface.
  if (viewport_) {
    viewport_->on_error = {};
    delete takeCentralWidget();
    viewport_ = nullptr;
  }
}
} // namespace race_editor
