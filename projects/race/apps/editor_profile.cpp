// Opt-in editor and file-operation profiling.
#include "editor_window.hpp"

namespace race_editor {
std::size_t Editor::profile_phase(std::uint64_t frame) const {
  if (file_profile_)
    return frame <= 60 ? 0 : 1;
  const auto base = 60 + profile_frames_;
  return frame <= 60 ? 0 : frame <= base ? 1 : frame <= base + 100 ? 2 : frame <= base + 120 ? 3 : 4;
}
QString Editor::profile_phase_name(std::uint64_t frame) const {
  constexpr std::array<const char *, 5> names{"warmup", "static", "editing", "save_load", "play"};
  return names[profile_phase(frame)];
}
bool Editor::profile_ready_for_frame() {
  ++profile_callbacks_;
  const bool visible = isVisible() && !isMinimized() && viewport_->isExposed() && viewport_->width() > 0 &&
                       viewport_->height() > 0;
  if (!visible) {
    if (profile_ready_) {
      error("Profile visibility changed after viewport stabilization");
      finish_profile(false);
    }
    return false;
  }
  const QSize physical(int(viewport_->width() * viewport_->devicePixelRatio()),
                       int(viewport_->height() * viewport_->devicePixelRatio()));
  if (!profile_ready_) {
    // Let native/Qt layout settle before any warmup or measured submission.
    if (physical != profile_candidate_size_) {
      profile_candidate_size_ = physical;
      profile_stable_callbacks_ = 1;
      return false;
    }
    if (++profile_stable_callbacks_ < 3)
      return false;
    profile_fixed_size_ = physical;
    profile_camera_ = viewport_->camera_pose();
    profile_draw_count_ = static_cast<std::uint64_t>(
        std::count_if(rows_.begin(), rows_.end(), [](const auto &row) { return row.visible; }));
    profile_ready_ = true;
    if (!profile_draw_count_)
      error("Profile requires an editable visible Box scene");
    // Ordinary Release supports the same timed workload. Unavailable allocation
    // counters stay explicitly unavailable; they are not measured zero traffic.
  }
  if (physical != profile_fixed_size_ || viewport_->camera_pose() != profile_camera_)
    error("Profile viewport dimensions or camera changed");
  if (profile_failed_) {
    finish_profile(false);
    return false;
  }
  return true;
}
bool Editor::eventFilter(QObject *watched, QEvent *event) {
  if (profile_.isEmpty() || profile_finished_ || !event->spontaneous())
    return false;
  auto *widget = qobject_cast<QWidget *>(watched);
  if (watched != viewport_ && watched != this && (!widget || !isAncestorOf(widget)))
    return false;
  switch (event->type()) {
  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonDblClick:
  case QEvent::Wheel:
  case QEvent::KeyPress:
  case QEvent::TouchBegin:
  case QEvent::TabletPress:
  case QEvent::Drop:
    error("Profile interrupted by user input");
    finish_profile(false);
    return true;
  default:
    return false;
  }
}
void Editor::collect_profile_gpu(const render::ViewportStats &stats) {
  if (!profile_device_epoch_)
    profile_device_epoch_ = stats.device_epoch;
  if (!profile_device_epoch_ || stats.device_epoch != profile_device_epoch_ ||
      stats.state != render::ViewportState::Ready) {
    error("Profile device epoch changed or became unavailable");
    return;
  }
  for (std::size_t i = 0; i < stats.gpu_sample_count; ++i) {
    const auto &sample = stats.gpu_samples[i];
    if (sample.device_epoch != profile_device_epoch_ || !sample.frame ||
        sample.frame >= profile_gpu_values_.size() || sample.frame > stats.submitted_frames ||
        !std::isfinite(sample.milliseconds) || sample.milliseconds < 0) {
      error("Invalid frame-identified GPU timestamp sample");
      continue;
    }
    auto &recorded = profile_gpu_values_[static_cast<std::size_t>(sample.frame)];
    if (recorded < 0) {
      recorded = sample.milliseconds;
      ++profile_gpu_count_;
    } else if (recorded != sample.milliseconds)
      error("GPU timestamp changed for an already recorded frame");
  }
}
void Editor::profile_interaction() {
  if (file_profile_) {
    file_profile_interaction();
    return;
  }
  const auto base = 60 + profile_frames_;
  if (frames_ == base) {
    auto row = std::find_if(rows_.begin(), rows_.end(), [](const auto &value) { return value.visible; });
    if (row == rows_.end()) {
      error("Profile has no Box to edit");
      return;
    }
    selection_ = row->id;
    profile_initial_x_ = row->local.translation.x;
    refresh();
    if (fields_[0][0]->value() != profile_initial_x_) {
      error("Profile Box X must be representable by the inspector");
      return;
    }
    profile_alternate_x_ = profile_initial_x_ <= fields_[0][0]->maximum() - .1 ? profile_initial_x_ + .1
                                                                               : profile_initial_x_ - .1;
  }
  if (frames_ >= base && frames_ < base + 100) {
    const bool edit = (frames_ - base) % 2 == 0;
    const auto generation = document_->generation();
    auto begin = Clock::now();
    double expected_x = profile_initial_x_;
    bool invoked = true;
    if (edit) {
      fields_[0][0]->setValue(profile_alternate_x_);
      expected_x = fields_[0][0]->value();
      invoked = expected_x != profile_initial_x_ &&
                QMetaObject::invokeMethod(fields_[0][0], "editingFinished", Qt::DirectConnection);
    } else
      undo_action_->trigger();
    const auto *row = selected_row();
    const bool passed =
        invoked && document_->generation() > generation && row && row->local.translation.x == expected_x;
    profile_operations_.append(QJsonObject{{"kind", edit ? "inspector_edit" : "inspector_undo"},
                                           {"milliseconds", milliseconds(Clock::now() - begin)},
                                           {"passed", passed}});
    if (passed) {
      if (edit)
        ++profile_edits_;
      else
        ++profile_undos_;
    } else
      error(edit ? "Profile inspector operation did not commit the expected position"
                 : "Profile Undo action did not restore the original position");
  }
  if (frames_ >= base + 100 && frames_ < base + 120) {
    const auto path = profile_ + "/roundtrip.scene.json";
    auto begin = Clock::now();
    auto saved = document_->save(*services_.files, path_string(path));
    profile_operations_.append(QJsonObject{
        {"kind", "save"}, {"milliseconds", milliseconds(Clock::now() - begin)}, {"passed", bool(saved)}});
    if (!check(saved))
      return;
    ++profile_saves_;
    begin = Clock::now();
    bool loaded = open(path);
    profile_operations_.append(QJsonObject{{"kind", "load_and_refresh"},
                                           {"milliseconds", milliseconds(Clock::now() - begin)},
                                           {"passed", loaded}});
    if (loaded)
      ++profile_loads_;
  }
  if (frames_ == base + 120) {
    auto begin = Clock::now();
    play_action_->trigger();
    profile_play_started_ = document_->playing();
    profile_operations_.append(QJsonObject{{"kind", "start_play"},
                                           {"milliseconds", milliseconds(Clock::now() - begin)},
                                           {"passed", profile_play_started_}});
    if (!profile_play_started_)
      error("Profile Play did not start");
  }
}
void Editor::profile_sample(double interval_ms, double cpu_ms) {
  const auto allocations = desktop::profile::allocation_snapshot();
  if (allocations.overflowed)
    error("Profile allocation counters overflowed");
  if (!viewport_->renderer()) {
    error("Profile renderer is missing");
    finish_profile(false);
    return;
  }
  const auto stats = viewport_->renderer()->stats();
  if (startup_ms_ == 0 && stats.submitted_frames)
    startup_ms_ = milliseconds(Clock::now() - process_start_);
  if (stats.submitted_frames != profile_submissions_ + 1 || stats.submitted_frames != frames_ ||
      stats.last_cpu_frame != stats.submitted_frames)
    error("Profile callback did not produce exactly one identified submission");
  profile_submissions_ = stats.submitted_frames;
  if (stats.width != static_cast<std::uint32_t>(profile_fixed_size_.width()) ||
      stats.height != static_cast<std::uint32_t>(profile_fixed_size_.height()) ||
      viewport_->camera_pose() != profile_camera_ || stats.last_draw_count != profile_draw_count_)
    error("Profile render dimensions, camera or object count changed");
  collect_profile_gpu(stats);
  if (frames_ % 30 == 0) {
    auto memory = platform::memory_info();
    if (memory) {
      profile_resident_ = memory.value().resident_bytes;
      profile_peak_ = memory.value().peak_resident_bytes;
    }
  }
  const auto base = 60 + profile_frames_;
  const auto expected_tick = !file_profile_ && frames_ > base + 120 ? frames_ - (base + 120) : 0;
  if (stats.simulation_tick != expected_tick || profile_play_ticks_ != expected_tick)
    error("Profile fixed-tick replay count differs from submitted Play frames");
  ++profile_phase_frames_[profile_phase(frames_)];
  append_profile_sample(interval_ms, cpu_ms, stats, allocations);
  if (profile_failed_) {
    finish_profile(false);
    return;
  }
  if (file_profile_) {
    if (frames_ == 600)
      finish_profile(true);
    return;
  }
  if (frames_ == base) {
    // Capture/drain outside measured CPU/allocation scopes. The following frame
    // interval still includes this explicit between-phase capture overhead.
    const auto begin = Clock::now();
    auto captured = viewport_->renderer()->capture(native_path(profile_ + "/static-scene.ppm"));
    profile_operations_.append(QJsonObject{{"kind", "static_capture"},
                                           {"milliseconds", milliseconds(Clock::now() - begin)},
                                           {"passed", bool(captured)}});
    check(captured);
    collect_profile_gpu(viewport_->renderer()->stats());
  }
  if (profile_failed_) {
    finish_profile(false);
    return;
  }
  if (frames_ >= base + 240)
    finish_profile(true);
}
void Editor::append_profile_sample(double interval_ms, double cpu_ms, const render::ViewportStats &stats,
                                   const desktop::profile::AllocationSnapshot &allocations) {
  profile_samples_.append(QJsonObject{
      {"frame", qint64(frames_)},
      {"callback", qint64(frames_)},
      {"submitted_frame_id", qint64(stats.submitted_frames)},
      {"last_cpu_frame", qint64(stats.last_cpu_frame)},
      {"phase", profile_phase_name(frames_)},
      {"interval_ms", interval_ms},
      {"cpu_frame_ms", cpu_ms},
      {"width", int(stats.width)},
      {"height", int(stats.height)},
      {"simulation_tick", qint64(stats.simulation_tick)},
      {"extraction_ms", viewport_->extraction_ms},
      {"render_submit_ms", viewport_->submission_ms},
      {"render_validation_ms", stats.last_validation_cpu_ms},
      {"render_record_ms", stats.last_record_cpu_ms},
      {"present_ms", stats.last_present_cpu_ms},
      {"inflight_wait_ms", stats.last_inflight_wait_cpu_ms},
      {"gpu_ms", stats.gpu_milliseconds},
      {"gpu_timed_frame", qint64(stats.last_timed_frame)},
      {"allocation_requests",
       qint64(allocations.overflowed ? 0 : allocations.requests - previous_allocations_.requests)},
      {"allocation_requested_bytes",
       qint64(allocations.overflowed ? 0
                                     : allocations.requested_bytes - previous_allocations_.requested_bytes)},
      {"resident_bytes", qint64(profile_resident_)},
      {"peak_resident_bytes", qint64(profile_peak_)}});
}
void Editor::finish_profile(bool capture_final) {
  if (file_profile_) {
    finish_file_profile(capture_final);
    return;
  }
  if (profile_finished_)
    return;
  profile_finished_ = true;
  if (capture_final && viewport_->renderer()) {
    const auto begin = Clock::now();
    auto captured = viewport_->renderer()->capture(native_path(profile_ + "/final-play.ppm"));
    profile_operations_.append(QJsonObject{{"kind", "final_capture_and_drain"},
                                           {"milliseconds", milliseconds(Clock::now() - begin)},
                                           {"passed", bool(captured)}});
    check(captured);
  }
  const auto stats = viewport_->renderer() ? viewport_->renderer()->stats() : render::ViewportStats{};
  if (viewport_->renderer())
    collect_profile_gpu(stats);
  const auto before_stop = Clock::now();
  const bool was_playing = document_->playing();
  auto stopped = document_->stop_play();
  profile_play_stopped_ = was_playing && bool(stopped) && !document_->playing();
  check(stopped);
  refresh_play_controls();
  profile_operations_.append(QJsonObject{{"kind", "stop_play"},
                                         {"milliseconds", milliseconds(Clock::now() - before_stop)},
                                         {"passed", profile_play_stopped_}});
  const auto expected_frames = 60 + profile_frames_ + 240;
  const std::array<std::uint64_t, 5> expected_phases{60, profile_frames_, 100, 20, 120};
  const bool workload_verified = !profile_failed_ && capture_final && profile_ready_ &&
                                 frames_ == expected_frames && stats.submitted_frames == expected_frames &&
                                 stats.last_cpu_frame == expected_frames &&
                                 profile_phase_frames_ == expected_phases && profile_edits_ == 50 &&
                                 profile_undos_ == 50 && profile_saves_ == 20 && profile_loads_ == 20 &&
                                 profile_play_ticks_ == 120 && profile_play_started_ && profile_play_stopped_;
  const bool gpu_complete = stats.gpu_timing_available && profile_gpu_count_ == stats.submitted_frames &&
                            stats.completed_frames == stats.submitted_frames;
  const auto allocations = desktop::profile::allocation_snapshot();
  const bool passed = workload_verified && gpu_complete && !allocations.overflowed && stats.hardware &&
                      stats.debug_errors == 0;
  QJsonArray gpu_samples, camera;
  QJsonObject actual_phases;
  for (std::size_t frame = 1; frame < profile_gpu_values_.size(); ++frame)
    if (profile_gpu_values_[frame] >= 0)
      gpu_samples.append(QJsonObject{{"frame", qint64(frame)},
                                     {"milliseconds", profile_gpu_values_[frame]},
                                     {"phase", profile_phase_name(frame)}});
  for (double value : profile_camera_)
    camera.append(value);
  constexpr std::array<const char *, 5> names{"warmup", "static", "editing", "save_load", "play"};
  for (std::size_t i = 0; i < names.size(); ++i)
    actual_phases[names[i]] = qint64(profile_phase_frames_[i]);
  const auto receipt = profile_receipt(stats, gpu_samples, camera, actual_phases, expected_frames,
                                       workload_verified, gpu_complete, allocations, passed);
  const auto bytes = QJsonDocument(receipt).toJson();
  QSaveFile file(profile_ + "/editor-profile.json");
  const bool written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(written && passed ? 0 : 2);
}
QJsonObject Editor::profile_receipt(const render::ViewportStats &stats, const QJsonArray &gpu_samples,
                                    const QJsonArray &camera, const QJsonObject &actual_phases,
                                    std::uint64_t expected_frames, bool workload_verified, bool gpu_complete,
                                    const desktop::profile::AllocationSnapshot &allocations,
                                    bool passed) const {
  QJsonObject receipt{
      {"schema", 2},
      {"application", "race_editor"},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"adapter", QString::fromStdString(stats.adapter)},
      {"hardware", stats.hardware},
      {"width", int(stats.width)},
      {"height", int(stats.height)},
      {"internal_width", int(stats.width)},
      {"internal_height", int(stats.height)},
      {"mode", "Raster box preview"},
      {"quality", "No production quality tier; directional Lambert + ambient; no AA or reconstruction"},
      {"quality_tier", "developer_box_preview"},
      {"native_resolution", true},
      {"render_scale", 1.0},
      {"frame_generation", false},
      {"antialiasing", "none"},
      {"present_sync_interval", 1},
      {"debug_layer", stats.debug_layer},
      {"camera_pose", camera},
      {"objects", qint64(rows_.size())},
      {"draw_count", qint64(profile_draw_count_)},
      {"startup_first_present_ms", startup_ms_},
      {"initial_scene_load_ms",
       initial_load_recorded_ ? QJsonValue(scene_load_ms_) : QJsonValue(QJsonValue::Null)},
      {"initial_scene_load_measured", initial_load_recorded_},
      {"initial_scene_source", initial_scene_source_},
      {"initial_scene_path", initial_scene_path_},
      {"package_scene_id", package_scene_id_},
      {"package_scene_revision", package_scene_revision_},
      {"warmup_frames", 60},
      {"static_measured_frames", qint64(profile_frames_)},
      {"samples", profile_samples_},
      {"operations", profile_operations_},
      {"actual_phase_frames", actual_phases},
      {"expected_submitted_frames", qint64(expected_frames)},
      {"submitted_frames", qint64(stats.submitted_frames)},
      {"completed_frames", qint64(stats.completed_frames)},
      {"timer_callbacks", qint64(profile_callbacks_)},
      {"layout_stabilization_callbacks", int(profile_stable_callbacks_)},
      {"inspector_edits", qint64(profile_edits_)},
      {"inspector_undos", qint64(profile_undos_)},
      {"saves", qint64(profile_saves_)},
      {"loads", qint64(profile_loads_)},
      {"play_ticks", qint64(profile_play_ticks_)},
      {"workload_verified", workload_verified},
      {"fixed_viewport_and_camera", profile_ready_ && !profile_failed_},
      {"input_policy", "Spontaneous modifying input aborts profiling; normal editor mode is unchanged"},
      {"gpu_samples", gpu_samples},
      {"gpu_sample_count", qint64(profile_gpu_count_)},
      {"gpu_samples_complete", gpu_complete},
      {"gpu_timing_available", stats.gpu_timing_available},
      {"latest_gpu_fields_deprecated", true},
      {"allocation_counting_available", desktop::profile::allocation_counting_available()},
      {"allocation_overflow", allocations.overflowed},
      {"allocation_scope",
       "Per timer workload interaction through render: EXE/static C++ requests only; excludes startup/event-loop/reporting work, Qt/SDL/driver DLL allocations; not live heap bytes"},
      {"memory_scope",
       "Process resident/peak sampled every 30 frames; includes profiling storage; private bytes unavailable"},
      {"timing_scope",
       "CPU wall scopes include stalls; intervals include event loop, timer, Present, reporting and between-phase capture overhead; GPU samples identify unique submitted command work, not whole application"},
      {"startup_timing_kind", "profile_launch_with_fixed_viewport_stabilization"},
      {"startup_scope",
       "Profile launch main entry through first successful present, including fixed viewport and three stable layout callbacks; not unmodified interactive startup. Initial file/package load includes dependency preparation and refresh; profile Scene and package starts also frame all objects"},
      {"editing_workload",
       "100 rendered frames alternate 50 committed inspector edits and 50 Undo QAction invocations; each checks generation and exact authored X; the 32-group undo history bound is unchanged"},
      {"play_workload",
       "Profile replay advances exactly one 1/60-second sample tick per Play frame; normal interactive mode uses elapsed bounded fixed steps"},
      {"debug_errors", qint64(stats.debug_errors)},
      {"debug_warnings", qint64(stats.debug_warnings)},
      {"errors", profile_errors_},
      {"passed", passed}};
  return receipt;
}
void Editor::file_profile_completed(desktop::PersistenceKind kind, double wait_ms, double publish_ms,
                                    double snapshot_ms, double worker_ms) {
  const bool save = kind == desktop::PersistenceKind::Save;
  profile_operations_.append(QJsonObject{
      {"kind", save ? "save" : "load"},
      {"start_frame", qint64(file_start_frame_)},
      {"completion_frame", qint64(frames_ + 1)},
      {"admission_ms", file_admission_ms_},
      {"until_poll_ms", file_async_ ? QJsonValue(wait_ms) : QJsonValue(QJsonValue::Null)},
      {"publication_and_ui_ms", file_async_ ? QJsonValue(publish_ms) : QJsonValue(QJsonValue::Null)},
      {"total_ms", milliseconds(Clock::now() - file_started_)},
      {"snapshot_ms", file_async_ ? QJsonValue(snapshot_ms) : QJsonValue(QJsonValue::Null)},
      {"worker_ms", file_async_ ? QJsonValue(worker_ms) : QJsonValue(QJsonValue::Null)},
      {"passed", true}});
  ++file_completions_;
  if (save)
    ++profile_saves_;
  else
    ++profile_loads_;
}
void Editor::file_profile_interaction() {
  // This is a separate workload. Legacy --profile retains its original
  // synchronous save/load pair and timing contract.
  if (frames_ < 60 || frames_ >= 540)
    return;
  const auto offset = (frames_ - 60) % 24;
  if (offset != 0 && offset != 12)
    return;
  if (persistence_->busy() || file_input_pending_) {
    error("File profile operation missed its next fixed slot");
    return;
  }
  const bool save = offset == 0;
  if (save) {
    if (rows_.empty()) {
      error("File profile has no editable object");
      return;
    }
    selection_ = rows_.front().id;
    refresh();
    if (profile_saves_ == 0)
      profile_initial_x_ = selected_row()->local.translation.x;
    if (selected_row()->local.translation.x != profile_initial_x_) {
      error("File profile load did not restore authored position");
      return;
    }
    profile_alternate_x_ = profile_initial_x_ + .1;
    const auto queued = Clock::now();
    const auto generation = document_->generation();
    file_input_pending_ = true;
    QTimer::singleShot(0, this, [this, queued, generation] {
      const auto begin = Clock::now();
      smoke_type(QString::number(profile_alternate_x_, 'f', 4));
      QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QCoreApplication::sendEvent(fields_[0][0], &key);
      const auto *row = selected_row();
      const bool passed = row && row->local.translation.x == profile_alternate_x_ &&
                          document_->generation() > generation && !pending_inspector();
      file_inputs_.append(QJsonObject{{"after_frame", qint64(frames_)},
                                      {"queue_delay_ms", milliseconds(begin - queued)},
                                      {"apply_ms", milliseconds(Clock::now() - begin)},
                                      {"passed", passed}});
      file_input_pending_ = false;
      if (passed)
        ++profile_edits_;
      else
        error("Queued inspector input failed");
    });
  }
  const auto kind = save ? desktop::PersistenceKind::Save : desktop::PersistenceKind::Load;
  const auto path = profile_ + "/roundtrip.scene.json";
  file_started_ = Clock::now();
  file_start_frame_ = frames_ + 1;
  if (file_async_) {
    if (!start_persistence(kind, path)) {
      error("File profile admission failed");
      return;
    }
    file_admission_ms_ = milliseconds(Clock::now() - file_started_);
  } else {
    // Same serializer, Files implementation and owner publication/refresh.
    // This branch exists only for a labelled same-source control workload.
    if (!commit_inspector()) {
      error("Synchronous file profile input invalid");
      return;
    }
    viewport_->cancel();
    const bool passed = save ? check(document_->save(*services_.files, path_string(path))) : open(path);
    file_admission_ms_ = milliseconds(Clock::now() - file_started_);
    if (!passed)
      return;
    if (save) {
      current_path_ = path;
      dirty_ = false;
      update_title();
    }
    file_profile_completed(kind, 0, 0, 0, 0);
  }
}
void Editor::finish_file_profile(bool capture_final) {
  if (profile_finished_)
    return;
  profile_finished_ = true;
  if (persistence_->busy() || file_input_pending_)
    error("File profile finished with retained work");
  if (capture_final && viewport_->renderer())
    check(viewport_->renderer()->capture(native_path(profile_ + "/final-scene.ppm")));
  const auto stats = viewport_->renderer() ? viewport_->renderer()->stats() : render::ViewportStats{};
  if (viewport_->renderer())
    collect_profile_gpu(stats);
  write_file_profile_scene();
  const bool workload = !profile_failed_ && capture_final && profile_ready_ && frames_ == 600 &&
                        profile_saves_ == 20 && profile_loads_ == 20 && profile_edits_ == 20 &&
                        file_completions_ == 40 && profile_play_ticks_ == 0;
  const bool gpu_complete = stats.gpu_timing_available && profile_gpu_count_ == 600 &&
                            stats.submitted_frames == 600 && stats.completed_frames == 600;
  const bool passed = workload && gpu_complete && stats.hardware && stats.debug_errors == 0;
  QJsonArray gpu, camera;
  for (std::size_t frame = 1; frame < profile_gpu_values_.size(); ++frame)
    if (profile_gpu_values_[frame] >= 0)
      gpu.append(QJsonObject{{"frame", qint64(frame)}, {"milliseconds", profile_gpu_values_[frame]}});
  for (double value : profile_camera_)
    camera.append(value);
  QJsonObject receipt{
      {"schema", 1},
      {"application", "race_editor_file_profile"},
      {"file_mode", file_async_ ? "async" : "sync"},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"adapter", QString::fromStdString(stats.adapter)},
      {"hardware", stats.hardware},
      {"width", int(stats.width)},
      {"height", int(stats.height)},
      {"camera_pose", camera},
      {"objects", qint64(rows_.size())},
      {"draw_count", qint64(profile_draw_count_)},
      {"native_resolution", true},
      {"render_scale", 1.0},
      {"antialiasing", "none"},
      {"frame_generation", false},
      {"present_sync_interval", 1},
      {"allocation_counting_available", desktop::profile::allocation_counting_available()},
      {"debug_layer", stats.debug_layer},
      {"debug_errors", qint64(stats.debug_errors)},
      {"debug_warnings", qint64(stats.debug_warnings)},
      {"expected_submitted_frames", 600},
      {"submitted_frames", qint64(stats.submitted_frames)},
      {"completed_frames", qint64(stats.completed_frames)},
      {"warmup_frames", 60},
      {"saves", qint64(profile_saves_)},
      {"loads", qint64(profile_loads_)},
      {"inspector_edits", qint64(profile_edits_)},
      {"workload_verified", workload},
      {"gpu_samples_complete", gpu_complete},
      {"samples", profile_samples_},
      {"operations", profile_operations_},
      {"input_samples", file_inputs_},
      {"gpu_samples", gpu},
      {"timing_scope",
       "CPU timer includes completion polling/publication, file admission and render. Queued input runs between frames and is separately timed. Wall time includes scheduling and driver waits. Completed GPU work is not displayed FPS."},
      {"workload",
       "600 rendered frames, 60 warmup, 20 saves at frames 61+24*n, 20 loads at 73+24*n, one queued inspector edit per save. Same work in both modes; async load becomes visible upon owner publication."},
      {"operation_scope",
       "Async admission includes snapshot/thread startup; worker covers encode/read/write/decode; publication includes install and UI refresh. Sync admission includes the entire operation; unavailable component scopes are null."},
      {"errors", profile_errors_},
      {"passed", passed}};
  const auto bytes = QJsonDocument(receipt).toJson();
  QSaveFile output(profile_ + "/file-profile.json");
  const bool written =
      output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(written && passed ? 0 : 2);
}
void Editor::write_file_profile_scene() {
  auto snapshot = document_->snapshot();
  if (snapshot) {
    auto encoded = scene::encode_json(snapshot.value(), *document_->schemas(), {});
    if (encoded) {
      QSaveFile file(profile_ + "/final.scene.json");
      const auto &bytes = encoded.value();
      if (!file.open(QIODevice::WriteOnly) ||
          file.write(bytes.data(), qsizetype(bytes.size())) != qsizetype(bytes.size()) || !file.commit())
        error("Final Scene receipt write failed");
    } else
      error(encoded.error().message);
  } else
    error(snapshot.error().message);
}
} // namespace race_editor
