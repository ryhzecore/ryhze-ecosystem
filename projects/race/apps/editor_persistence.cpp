// Scene files: open, package load, save, background persistence, recovery and close.
#include "editor_window.hpp"

namespace race_editor {
bool Editor::open(const QString &path, bool initial_load) {
  const auto begin = Clock::now();
  viewport_->cancel();
  const auto stamp = document_->persistence_stamp();
  auto decoded = desktop::Document::decode(*services_.files, path_string(path), *document_->schemas());
  if (!decoded) {
    error(decoded.error().message);
    return false;
  }
  auto refs = desktop::mesh_references(decoded.value());
  if (!refs) {
    error(refs.error().message);
    return false;
  }
  auto material_refs = desktop::material_references(decoded.value());
  if (!material_refs) {
    error(material_refs.error().message);
    return false;
  }
  auto candidate = desktop::MeshSet::prepare(meshes_->files(), refs.value(), meshes_, material_refs.value());
  if (!candidate) {
    error(candidate.error().message);
    return false;
  }
  if (!check(candidate.value()->validate_scene(decoded.value())))
    return false;
  if (!check(document_->install(decoded.value(), stamp)))
    return false;
  meshes_ = std::move(candidate).value();
  current_path_ = path.endsWith(".rscene", Qt::CaseInsensitive) ? QString{} : path;
  dirty_ = false;
  selection_ = {};
  refresh();
  if (!profile_.isEmpty() && initial_load)
    viewport_->frame_all();
  if (initial_load) {
    scene_load_ms_ = milliseconds(Clock::now() - begin);
    initial_load_recorded_ = true;
    initial_scene_source_ = "file";
    initial_scene_path_ = path;
  }
  return true;
}
bool Editor::open_package(const QString &path, bool initial_load) {
  if (!initial_load)
    return start_persistence(desktop::PersistenceKind::PackageLoad, path);
  const auto begin = Clock::now();
  if (file_busy() || document_->playing())
    return false;
  const auto stamp = document_->persistence_stamp();
  auto prepared = desktop::read_package(*services_.files, path_string(path), *document_->schemas());
  if (!prepared) {
    error(prepared.error().message);
    return false;
  }
  if (!check(document_->install(prepared.value().scene, stamp)))
    return false;
  viewport_->cancel();
  meshes_ = std::move(prepared.value().meshes);
  assets_path_.clear();
  current_path_.clear();
  dirty_ = false;
  selection_ = {};
  refresh();
  viewport_->frame_all();
  package_scene_id_ = QString::fromStdString(assets::id_text(prepared.value().scene_key.id));
  package_scene_revision_ = QString::fromStdString(assets::hex(prepared.value().scene_key.revision));
  if (initial_load) {
    scene_load_ms_ = milliseconds(Clock::now() - begin);
    initial_load_recorded_ = true;
    initial_scene_source_ = "package";
    initial_scene_path_ = path;
  }
  statusBar()->showMessage("Content package loaded", 5000);
  return true;
}
bool Editor::save(bool as) {
  if (file_busy() || !commit_inspector())
    return false;
  QString path = current_path_;
  if (as || path.isEmpty())
    path = QFileDialog::getSaveFileName(this, "Save editable Scene", path, "Scene JSON (*.json)");
  if (path.isEmpty())
    return false;
  return start_persistence(desktop::PersistenceKind::Save, path);
}
bool Editor::content_operation_busy() const {
  return persistence_->busy() || mesh_checkpoint_.busy() || project_browser_.busy() ||
         mesh_reimport_.busy() || surface_reimport_.busy() || surface_checkpoint_.busy() ||
         project_authoring_.busy() || material_assignment_.busy();
}
bool Editor::file_busy() {
  if (!content_operation_busy())
    return false;
  statusBar()->showMessage("A content or Scene file operation is in progress. Editing remains available.",
                           10000);
  return true;
}
bool Editor::start_persistence(desktop::PersistenceKind kind, const QString &path) {
  if (file_busy())
    return false;
  if (!admit_persistence(kind, path))
    return false;
  desktop::PersistenceTask::Prepare prepare;
  desktop::PersistenceTask::Decode decode;
  pending_meshes_.reset();
  pending_package_key_.reset();
  if (kind == desktop::PersistenceKind::PackageLoad) {
    pending_meshes_ = std::make_shared<std::shared_ptr<const desktop::MeshSet>>();
    pending_package_key_ = std::make_shared<assets::pak::Key>();
    decode = [out = pending_meshes_,
              key = pending_package_key_](platform::Files &files, std::string_view source,
                                          const scene::Schemas &schemas) -> Result<scene::Document> {
      auto ready = desktop::read_package(files, source, schemas);
      if (!ready)
        return ready.error();
      *out = std::move(ready.value().meshes);
      *key = ready.value().scene_key;
      return std::move(ready.value().scene);
    };
  } else if (desktop::reads_scene(kind)) {
    pending_meshes_ = std::make_shared<std::shared_ptr<const desktop::MeshSet>>();
    prepare = [root = meshes_->files(), reuse = meshes_,
               out = pending_meshes_](const scene::Document &doc) -> Result<void> {
      auto refs = desktop::mesh_references(doc);
      if (!refs)
        return refs.error();
      auto materials = desktop::material_references(doc);
      if (!materials)
        return materials.error();
      auto ready = desktop::MeshSet::prepare(root, refs.value(), reuse, materials.value());
      if (!ready)
        return ready.error();
      auto admitted = ready.value()->validate_scene(doc);
      if (!admitted)
        return admitted;
      *out = std::move(ready).value();
      return {};
    };
  }
  auto started =
      persistence_->start(*document_, kind, path_string(path), std::move(prepare), std::move(decode));
  if (!started) {
    pending_meshes_.reset();
    pending_package_key_.reset();
    error(started.error().message);
    return false;
  }
  persistence_kind_ = kind;
  refresh_asset_controls();
  cancel_load_action_->setEnabled(desktop::reads_scene(kind));
  statusBar()->showMessage(kind == desktop::PersistenceKind::Recover ? "Recovering authoring snapshot..."
                           : kind == desktop::PersistenceKind::Checkpoint
                               ? "Saving authoring recovery snapshot..."
                           : desktop::reads_scene(kind)             ? "Loading Scene..."
                           : kind == desktop::PersistenceKind::Cook ? "Cooking Scene..."
                                                                    : "Saving Scene...");
  return true;
}
bool Editor::admit_persistence(desktop::PersistenceKind kind, const QString &path) {
  if (kind == desktop::PersistenceKind::Checkpoint) {
    // A checkpoint is deliberately independent of unpublished text/previews.
    // Never replace the user's current file through spelling aliases, symlinks
    // or existing hard links on this platform.
    std::error_code ec;
    const bool equivalent = !current_path_.isEmpty() &&
                            std::filesystem::equivalent(native_path(path), native_path(current_path_), ec);
    auto normalized = [](const QString &value) {
      QFileInfo file(value);
      const auto canonical = file.canonicalFilePath();
      return canonical.isEmpty() ? QDir::cleanPath(file.absoluteFilePath()) : canonical;
    };
    if (!current_path_.isEmpty() &&
        (equivalent || normalized(path).compare(normalized(current_path_), Qt::CaseInsensitive) == 0)) {
      error("Choose a recovery destination separate from the current Scene file");
      return false;
    }
  } else {
    if (!commit_inspector())
      return false;
    viewport_->cancel();
  }
  return true;
}
bool Editor::choose_checkpoint(bool smoke_cancel) {
  if (file_busy())
    return false;
  QFileDialog dialog(this, "Save published authoring snapshot", {}, "RACE recovery Scene (*.scene.json)");
  dialog.setAcceptMode(QFileDialog::AcceptSave);
  dialog.setDefaultSuffix("scene.json");
  if (!smoke_.isEmpty()) {
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.selectFile(smoke_ + "/recovery.scene.json");
    QTimer::singleShot(0, &dialog, [&dialog, smoke_cancel] {
      QMetaObject::invokeMethod(&dialog, smoke_cancel ? "reject" : "accept", Qt::DirectConnection);
    });
  }
  return dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty() &&
         start_persistence(desktop::PersistenceKind::Checkpoint, dialog.selectedFiles().front());
}
void Editor::recover_snapshot() {
  if (document_->playing()) {
    error("Stop Play before recovering an authoring snapshot");
    return;
  }
  after_discard([this] {
    auto path =
        QFileDialog::getOpenFileName(this, "Recover authoring snapshot", {}, "RACE recovery Scene (*.json)");
    if (!path.isEmpty())
      start_persistence(desktop::PersistenceKind::Recover, path);
  });
}
void Editor::poll_persistence() {
  if (!persistence_->busy())
    return;
  const auto completion_begin = Clock::now();
  const bool unpublished = pending_inspector() || viewport_->previewing();
  if (unpublished) {
    after_save_ = {};
    if (desktop::reads_scene(persistence_kind_)) {
      auto ignored = persistence_->cancel_load();
      (void)ignored;
    }
  }
  auto polled = persistence_->poll(*document_);
  if (!polled) {
    error(polled.error().message);
    return;
  }
  if (!polled.value())
    return;
  auto completion = std::move(*polled.value());
  if (completion.kind == desktop::PersistenceKind::PackageLoad && package_smoke_stage_ == 1 &&
      completion.result)
    package_worker_ms_ = completion.worker_ms;
  cancel_load_action_->setEnabled(false);
  refresh_asset_controls();
  if (completion.result && desktop::reads_scene(completion.kind) && pending_meshes_ && *pending_meshes_)
    meshes_ = std::move(*pending_meshes_);
  pending_meshes_.reset();
  auto continuation = std::move(after_save_);
  after_save_ = {};
  if (smoke_expected_persistence_error_) {
    smoke_check("expected_background_error_" + QString::number(persistence_smoke_stage_),
                !completion.result && completion.result.error().code == *smoke_expected_persistence_error_);
    if (!completion.result) {
      pending_package_key_.reset();
      auto item = smoke_checks_.last().toObject();
      item["actual_error_code"] = int(completion.result.error().code);
      item["actual_error"] = QString::fromUtf8(completion.result.error().message);
      smoke_checks_.last() = item;
    }
    smoke_expected_persistence_error_.reset();
    ++persistence_smoke_stage_;
    return;
  }
  if (!completion.result) {
    pending_package_key_.reset();
    error(completion.result.error().message);
    if (!smoke_.isEmpty())
      persistence_smoke_stage_ = 20;
    return;
  }
  publish_persistence(completion, unpublished);
  if (file_profile_)
    file_profile_completed(completion.kind, milliseconds(completion_begin - file_started_),
                           milliseconds(Clock::now() - completion_begin), completion.snapshot_ms,
                           completion.worker_ms);
  if (!smoke_.isEmpty() && persistence_smoke_stage_) {
    smoke_check("background_file_operation_" + QString::number(persistence_smoke_stage_), true);
    ++persistence_smoke_stage_;
  }
  if (completion.kind == desktop::PersistenceKind::Save && continuation && completion.matches_current &&
      !unpublished)
    continuation();
}
void Editor::publish_persistence(const desktop::PersistenceCompletion &completion, bool unpublished) {
  const auto path = QString::fromUtf8(completion.path.data(), static_cast<qsizetype>(completion.path.size()));
  if (desktop::reads_scene(completion.kind)) {
    const bool recovered = completion.kind == desktop::PersistenceKind::Recover;
    const bool packaged = completion.kind == desktop::PersistenceKind::PackageLoad;
    viewport_->cancel();
    current_path_ = recovered || packaged || path.endsWith(".rscene", Qt::CaseInsensitive) ? QString{} : path;
    if (packaged) {
      assets_path_.clear();
      package_scene_id_ = QString::fromStdString(assets::id_text(pending_package_key_->id));
      package_scene_revision_ = QString::fromStdString(assets::hex(pending_package_key_->revision));
      viewport_->frame_all();
    }
    pending_package_key_.reset();
    dirty_ = recovered;
    selection_ = {};
    refresh();
    statusBar()->showMessage(recovered ? "Snapshot recovered as an unsaved Scene" : "Scene loaded", 5000);
  } else if (completion.kind == desktop::PersistenceKind::Save) {
    current_path_ = path;
    if (completion.matches_current && !unpublished)
      dirty_ = false;
    update_title();
    statusBar()->showMessage(completion.matches_current && !unpublished
                                 ? "Scene saved"
                                 : "Snapshot saved; newer edits remain unsaved",
                             10000);
  } else
    statusBar()->showMessage(completion.kind == desktop::PersistenceKind::Checkpoint
                                 ? "Authoring snapshot saved; editor state preserved"
                                 : "Runtime Scene cooked",
                             5000);
}
void Editor::after_discard(std::function<void()> action) {
  if (file_busy() || !commit_inspector())
    return;
  if (document_->playing()) {
    toggle_play();
    if (document_->playing())
      return;
  }
  if (!dirty_) {
    action();
    return;
  }
  auto answer = QMessageBox::question(this, "Unsaved Scene", "Save your Scene before continuing?",
                                      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (answer == QMessageBox::Discard)
    action();
  else if (answer == QMessageBox::Save && save(false))
    after_save_ = std::move(action);
}
void Editor::closeEvent(QCloseEvent *event) {
  if (!profile_.isEmpty() && !closing_smoke_) {
    error("Profile closed before workload completion");
    finish_profile(false);
    event->accept();
    return;
  }
  if (!closing_smoke_ &&
      (persistence_->busy() || mesh_checkpoint_.busy() || project_browser_.busy() || mesh_reimport_.busy() ||
       surface_reimport_.busy() || surface_checkpoint_.busy() || project_authoring_.busy() ||
       material_assignment_.busy() || dirty_ || pending_inspector())) {
    event->ignore();
    after_discard([this] {
      dirty_ = false;
      close();
    });
    return;
  }
  viewport_->cancel();
  auto stopped = document_->stop_play();
  (void)stopped;
  if (smoke_.isEmpty() && profile_.isEmpty()) {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("layout-workspace-v3", saveState());
  }
  event->accept();
}
} // namespace race_editor
