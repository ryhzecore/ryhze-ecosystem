#pragma once
// The editor main window. Member definitions are grouped by workflow in editor*.cpp.
#include "editor_viewport.hpp"

namespace race_editor {
class Editor final : public QMainWindow {
  std::unique_ptr<desktop::Document> document_;
  platform::Services services_;
  std::shared_ptr<const desktop::MeshSet> meshes_;
  std::shared_ptr<std::shared_ptr<const desktop::MeshSet>> pending_meshes_;
  QString assets_path_, mesh_product_, material_product_;
  desktop::MaterialAssignmentTask material_assignment_;
  int material_stage_{};
  QAction *material_action_{}, *clear_material_action_{}, *cancel_material_action_{};
  QLineEdit *material_revision_{};
  bool mesh_test_{};
  int mesh_stage_{}, package_smoke_stage_{};
  std::uint64_t package_heartbeats_{}, package_pending_heartbeats_{};
  double package_sync_ms_{}, package_async_ms_{}, package_worker_ms_{}, package_admission_ms_{},
      package_max_callback_gap_ms_{};
  Clock::time_point package_previous_callback_{};
  Clock::time_point package_started_;
  std::shared_ptr<const desktop::MeshSet> package_smoke_meshes_;
  std::vector<desktop::MeshReference> mesh_expected_;
  QAction *assets_action_{}, *mesh_action_{};
  QWidget *mesh_details_{};
  QLineEdit *asset_id_{}, *asset_revision_{};
  Viewport *viewport_{};
  QTreeWidget *hierarchy_{};
  QWidget *inspector_{};
  QLabel *status_{};
  std::array<std::array<QDoubleSpinBox *, 3>, 5> fields_{};
  // P03.04 Scene-wide lighting controls (Properties > Lighting); values publish through Document::set_lighting.
  QWidget *lighting_panel_{};
  QCheckBox *physical_toggle_{};
  QLabel *output_status_{};
  std::array<InspectorNumber *, 10> light_fields_{};
  desktop::LightingSettings lighting_;
  std::array<std::array<double, 3>, 5> displayed_{};
  std::array<std::array<QString, 3>, 5> displayed_text_{};
  std::array<Vec, 5> inspected_values_{};
  std::vector<desktop::ObjectRow> rows_;
  PersistentId selection_;
  std::uint64_t inspected_generation_{};
  QAction *play_action_{}, *add_action_{}, *undo_action_{}, *clear_history_action_{};
  QAction *save_action_{}, *redo_action_{};
  QString current_path_, smoke_;
  std::filesystem::path shaders_;
  bool dirty_{}, refreshing_{}, closing_smoke_{};
  std::uint64_t frames_{};
  std::uint64_t simulation_ticks_{};
  QElapsedTimer play_clock_;
  double play_accumulator_{};
  QJsonArray smoke_checks_;
  QString profile_;
  std::uint64_t profile_frames_{600};
  Clock::time_point process_start_, previous_frame_;
  double scene_load_ms_{}, startup_ms_{};
  QJsonArray profile_samples_, profile_operations_;
  desktop::profile::AllocationSnapshot previous_allocations_{};
  std::uint64_t profile_resident_{}, profile_peak_{};
  bool profile_failed_{};
  bool initial_load_recorded_{};
  QString initial_scene_source_{"new_document"}, initial_scene_path_, package_scene_id_,
      package_scene_revision_;
  std::shared_ptr<assets::pak::Key> pending_package_key_;
  double profile_initial_x_{}, profile_alternate_x_{};
  QJsonArray profile_errors_;
  std::vector<double> profile_gpu_values_;
  std::uint64_t profile_device_epoch_{};
  std::array<std::uint64_t, 5> profile_phase_frames_{};
  std::array<double, 6> profile_camera_{};
  QSize profile_candidate_size_, profile_fixed_size_;
  std::uint64_t profile_callbacks_{}, profile_submissions_{}, profile_gpu_count_{}, profile_draw_count_{};
  std::uint64_t profile_edits_{}, profile_undos_{}, profile_saves_{}, profile_loads_{}, profile_play_ticks_{};
  unsigned profile_stable_callbacks_{};
  bool profile_ready_{}, profile_finished_{}, profile_play_started_{}, profile_play_stopped_{};
  std::unique_ptr<desktop::PersistenceTask> persistence_;
  desktop::ProjectAuthoringTask project_authoring_;
  bool authoring_test_{}, authoring_uv_test_{}, authoring_dialog_cancel_{}, smoke_browser_click_{};
  int authoring_stage_{};
  QJsonArray authoring_metrics_, authoring_checks_;
  bool content_test_{}, content_authoring_retry_{};
  int content_stage_{};
  std::shared_ptr<const desktop::MeshSet> content_color_meshes_;
  QJsonArray content_metrics_;
  std::optional<assets::surface::Reference> authoring_texture_, authoring_scalar_texture_,
      authoring_texture_draft_;
  std::optional<surface_recipe::Profile> authoring_surface_profile_;
  QString authoring_recipe_root_, authoring_source_root_, authoring_product_root_, authoring_checkpoint_root_;
  QString authoring_index_, authoring_source_leaf_, authoring_asset_;
  mesh_pipeline::Profile authoring_profile_{mesh_pipeline::Profile::Geometry};
  desktop::ProjectBrowserTask project_browser_;
  std::shared_ptr<const desktop::ProjectBrowserView> project_view_;
  desktop::ui::ContentBrowser *content_browser_{};
  QDockWidget *content_dock_{};
  std::uint64_t project_epoch_{};
  std::optional<ErrorCode> browser_expected_error_;
  QJsonArray browser_metrics_;
  desktop::MeshReimportTask mesh_reimport_{
      {},
      desktop::editor_reimport_workers(
          path_string(QCoreApplication::applicationDirPath() + "/race_mesh_import_worker.exe"))};
  std::shared_ptr<const desktop::MeshReimportCheckpoint> reimport_checkpoint_;
  std::optional<desktop::MeshReimportCompletion> reimport_completion_;
  std::optional<ErrorCode> reimport_expected_error_;
  desktop::MeshReimportPhase reimport_phase_{desktop::MeshReimportPhase::Idle};
  QString reimport_source_, reimport_checkpoints_;
  QJsonArray reimport_metrics_;
  std::uint64_t reimport_callbacks_{};
  double reimport_gap_ms_{};
  Clock::time_point reimport_callback_{};
  desktop::SurfaceReimportTask surface_reimport_;
  desktop::SurfaceCheckpointTask surface_checkpoint_;
  std::shared_ptr<const desktop::SurfaceReimportCheckpoint> surface_capture_;
  std::optional<desktop::SurfaceReimportCompletion> surface_import_completion_;
  std::optional<desktop::SurfaceCheckpointCompletion> surface_adoption_completion_;
  desktop::MeshCheckpointTask mesh_checkpoint_;
  QAction *update_mesh_action_{};
  QAction *cancel_mesh_update_action_{};
  QString checkpoint_project_, checkpoint_file_;
  bool checkpoint_test_{};
  int checkpoint_stage_{};
  std::optional<desktop::MeshCheckpointCompletion> checkpoint_completion_;
  std::optional<ErrorCode> checkpoint_expected_error_;
  desktop::MeshReference checkpoint_before_;
  std::optional<desktop::MaterialReference> checkpoint_material_;
  int checkpoint_material_stage_{};
  PersistentId checkpoint_selected_, checkpoint_other_;
  QString checkpoint_authoring_path_;
  std::uint64_t checkpoint_generation_{}, checkpoint_callbacks_{};
  double checkpoint_gap_ms_{};
  Clock::time_point checkpoint_callback_{};
  QJsonArray checkpoint_metrics_;
  QAction *cancel_load_action_{};
  std::function<void()> after_save_;
  int persistence_smoke_stage_{};
  desktop::PersistenceKind persistence_kind_{};
  std::optional<ErrorCode> smoke_expected_persistence_error_;
  QString smoke_pending_text_, smoke_selected_text_;
  int smoke_cursor_{};
  std::uint64_t smoke_generation_{};
  bool smoke_deferred_action_{};
  QString smoke_authoring_path_;
  PersistentId smoke_selection_;
  bool smoke_dirty_{};
  desktop::startup::Witness *startup_{};
  bool file_profile_{}, file_async_{}, file_input_pending_{};
  Clock::time_point file_started_;
  double file_admission_ms_{};
  std::uint64_t file_start_frame_{}, file_completions_{};
  QJsonArray file_inputs_;

public:
  Editor(std::unique_ptr<desktop::Document> doc, platform::Services services, QString shaders, QString smoke,
         QString profile = {}, std::uint64_t profile_frames = 600,
         Clock::time_point process_start = Clock::now(), QString file_mode = {}, bool mesh_test = false,
         QString mesh_product = {}, QString checkpoint_project = {}, QString checkpoint_file = {},
         bool ui_test = false, bool authoring_test = false, QString material_product = {},
         bool authoring_uv_test = false, bool content_test = false, bool epoch_test = false,
         bool light_test = false, desktop::startup::Witness *startup = nullptr);
  void initialize_profile_storage();
  void startup_adopted();
  ~Editor() override;
  bool open(const QString &path, bool initial_load = false);
  bool open_package(const QString &path, bool initial_load = false);
  bool choose_assets(const QString &path);
  bool add_native_mesh(const QString &path);

private:
  // Constructor parts, called once in this order by Editor::Editor.
  QWidget *build_viewport_panel();
  QDockWidget *build_hierarchy_dock();
  void build_inspector();
  void add_inspector_vector(QFormLayout *form, const std::array<QString, 5> &names, std::size_t group);
  void add_material_controls(QFormLayout *form);
  void build_lighting_panel();
  void add_lighting_fields(QFormLayout *light_form);
  QDockWidget *build_properties_dock();
  void build_content_dock(QDockWidget *hierarchy_dock);
  QMenu *build_file_menu();
  QMenu *build_edit_menu();
  QMenu *build_view_menu(QWidget *container, QDockWidget *hierarchy_dock, QDockWidget *properties);
  QToolBar *build_header(const std::array<QMenu *, 3> &menus);
  void build_scene_tools(QMenu *view);
  void connect_editor_signals();
  void start_frame_timer();
  void start_test_heartbeats();
  // One 16 ms editor frame: worker polls, Play steps, render, profile and smoke steps.
  void frame_tick();
  void poll_workers();
  void advance_play(double elapsed);
  void update_status_text();
  void run_smoke_step();
  void update_selected_content();
  std::vector<desktop::MaterialReference> retained_materials() const;
  void choose_material();
  void poll_material_assignment();
  QAction *action(QMenu *menu, QString name, QKeySequence shortcut, std::function<void()> callback);
  bool check(Result<void> result);
  void error(QString message);
  desktop::ObjectRow *selected_row();
  void refresh();
  void refresh_asset_controls();
  bool pending_inspector() const;
  void update_title();
  void refresh_inspector();
  // The Properties scroll area encloses the inspector and the Lighting panel through one body
  // widget; walk up instead of assuming a fixed depth.
  QScrollArea *properties_scroll();
  void refresh_lighting_controls();
  void apply_lighting_controls();
  void apply_inspector(std::size_t group);
  bool commit_inspector();
  void changed();
  bool select_object(PersistentId id);
  bool new_scene();
  bool clear_scene_history();
  void add_box();
  void clear_history();
  void refresh_play_controls();
  void toggle_play();
  bool save(bool as);
  bool content_operation_busy() const;
  bool file_busy();
  bool start_persistence(desktop::PersistenceKind kind, const QString &path);
  // start_persistence part: the checkpoint destination check, or the inspector commit for other kinds.
  bool admit_persistence(desktop::PersistenceKind kind, const QString &path);
  bool choose_checkpoint(bool smoke_cancel = false);
  void recover_snapshot();
  void poll_persistence();
  // poll_persistence part: publishes a successful completion to the window state and status bar.
  void publish_persistence(const desktop::PersistenceCompletion &completion, bool unpublished);
  bool content_can_author() const;
  void smoke_dialog_curves(QDialog &dialog, const QString &tag, QLineEdit *field);
  void smoke_profile_choices(QComboBox &profile);
  void smoke_profile_capture(QComboBox &profile, QAbstractItemView *view, QWidget *viewport, QWidget *popup,
                             int row, const QString &name);
  void choose_project_authoring(bool add_recipe);
  // Authoring dialog parts, called by choose_project_authoring in this order; accept and the test hooks run
  // from the open dialog.
  struct AuthoringDialog;
  void authoring_dialog_fields(AuthoringDialog &ui);
  void authoring_texture_candidates(AuthoringDialog &ui) const;
  void refresh_authoring_textures(AuthoringDialog &ui) const;
  void authoring_dialog_folders(AuthoringDialog &ui);
  void authoring_recipe_rows(AuthoringDialog &ui);
  void authoring_restore_draft(AuthoringDialog &ui) const;
  void authoring_accept(AuthoringDialog &ui);
  void authoring_content_smoke(AuthoringDialog &ui);
  void authoring_content_smoke_fields(AuthoringDialog &ui, surface_recipe::Profile requested);
  void authoring_dialog_smoke(AuthoringDialog &ui);
  void authoring_retain_draft(AuthoringDialog &ui);
  void cancel_project_authoring();
  void poll_project_authoring();
  void choose_project_browser();
  QString project_path() const;
  content_project::Kind selected_content_kind() const;
  bool content_can_update();
  void refresh_content_context();
  bool content_can_reimport() const;
  const desktop::MeshReimportCheckpoint *matching_checkpoint() const;
  void choose_reimport();
  // choose_reimport parts: one folder row, and the content and checkpoint smokes' dialog hooks.
  void reimport_folder(QDialog &dialog, QFormLayout *form, const QString &label, QLineEdit *field);
  void reimport_dialog_smoke(QDialog &dialog, QLineEdit *source, QLineEdit *checkpoints, QLineEdit *products,
                             QDialogButtonBox *buttons, QPushButton *import_button);
  bool start_reimport(const QString &source, const QString &checkpoints);
  void cancel_reimport();
  void poll_reimport();
  const desktop::SurfaceReimportCheckpoint *matching_surface_capture() const;
  void poll_surface_import();
  bool start_surface_adoption(const QString &path);
  void poll_surface_adoption();
  bool start_project_browser(const QString &path);
  void cancel_project_browser();
  void poll_project_browser();
  bool update_from_browser(const QString &checkpoint);
  void cancel_mesh_update();
  bool start_mesh_update(const QString &project, const QString &checkpoint,
                         std::optional<assets::Digest> expected_project = {});
  void poll_mesh_update();
  void after_discard(std::function<void()> action);
  void smoke_check(QString name, bool passed);
  bool smoke_passed() const;

  // Bounded native UI acceptance; normal editing keeps this state idle.
  bool ui_test_{};
  int ui_layout_{}, ui_settle_{}, ui_step_{};
  QJsonArray ui_layouts_;
  std::string ui_before_, ui_after_;
  std::uint64_t ui_play_generation_{};
  bool ui_dirty_before_{};
  // Concrete QObject ownership removes the smoke observer before QWidget child teardown.
  std::unique_ptr<QObject> ui_menu_trace_;

  bool epoch_test_{};
  int epoch_stage_{};
  bool light_test_{};
  int light_stage_{};
  std::string light_legacy_, light_physical_;
  std::string epoch_original_, epoch_authored_;
  std::array<double, 6> epoch_camera_{};
  std::uint64_t epoch_generation_{}, epoch_number_{};
  PersistentId epoch_selection_;
  render::WindowsViewport *epoch_owner_{};
  std::optional<render::WindowsViewport::ImageToken> epoch_token_;
  void epoch_smoke_step();
  // Epoch and light smoke parts; the stage parts run inside their step's try block.
  std::string smoke_scene_json();
  void epoch_smoke_retire_surface();
  void epoch_smoke_restore_surface();
  void epoch_smoke_finish();

  // P03.04 editor smoke: explicit opt-in controls, undo/redo, JSON and cooked persistence through the
  // background worker, and Play isolation, each observed through the real widgets and Document.
  void light_smoke_step();
  void light_smoke_physical_controls();
  void light_smoke_undo_redo();
  void light_smoke_play();
  void light_smoke_finish();

  void ui_smoke_step();
  // UI smoke parts: shared widget lookups, the layout pass, menus, steps 12-27 and the receipt.
  QToolButton *ui_button(const char *n) const;
  QRect ui_rect(QWidget *w) const;
  std::string ui_scene_json();
  void ui_trigger(const char *n);
  void ui_smoke_layout(QToolBar *header, QLabel *brand, QLabel *badge);
  void ui_smoke_layout_tools(const QString &tag);
  QJsonArray ui_smoke_layout_panels(const QString &tag);
  std::pair<QString, QString> ui_smoke_layout_actions(const QString &tag, QToolBar *header, QLabel *brand,
                                                      bool ordered);
  void ui_smoke_menu();
  void ui_smoke_action_step(int step);
  void ui_smoke_view_menu_step(int step, QToolBar *header);
  void ui_smoke_gizmo_step(int step);
  void ui_smoke_viewport_step(int step);
  void ui_smoke_finish();

  void checkpoint_capture(const QString &leaf);
  void browser_button(const QString &text);
  void smoke_content_curves();
  void authoring_check(const QString &name, bool passed);
  void authoring_smoke_step();
  // Authoring smoke parts: the shared preservation check and stage 93.
  bool authoring_preserved() const;
  void authoring_smoke_fixtures();
  QString browser_field(const char *name);
  void browser_check_revisions(const QString &phase, int resident_count);
  void browser_smoke_step();
  // Browser smoke parts: the shared row key and preservation check, then stages 99-100, 101-104, 105-107
  // and 108-109.
  void browser_key(QListWidget *tree, int value);
  bool browser_preserved() const;
  void browser_smoke_open(QListWidget *tree);
  void browser_smoke_refresh(QListWidget *tree);
  void browser_smoke_reimport(QListWidget *tree);
  void browser_smoke_reimport_selection(QListWidget *tree);
  void checkpoint_smoke_step();
  // Checkpoint smoke parts: the shared update request, then stage 0, stages 1, 11, 2, 3 and 30, 4 and 5.
  bool checkpoint_next(int stage);
  void checkpoint_smoke_setup();
  void checkpoint_smoke_cancelled_requests();
  void checkpoint_smoke_update();
  void checkpoint_smoke_loose_control();
  void checkpoint_smoke_finish();

  void content_smoke_step();
  // Content smoke parts: the shared check and action helpers, then stages 0-4, 5-7, 8-12 and 13-15.
  bool content_ready();
  bool content_require(const char *name, bool passed);
  bool content_act(const char *name);
  void content_smoke_texture_stages();
  void content_smoke_material_stages();
  void content_smoke_scalar_stages();
  void content_smoke_scalar_material_stages();
  void mesh_smoke_step();
  // Mesh smoke parts: package stages 0-3, mesh stage 0 (two parts), stages 1, 4 and 5, and the receipt.
  QString mesh_smoke_path(const char *leaf) const;
  void mesh_smoke_package_open();
  void mesh_smoke_package_corrupt();
  void mesh_smoke_package_missing();
  void mesh_smoke_package_end();
  void mesh_smoke_setup();
  void mesh_smoke_inspect();
  void mesh_smoke_layout();
  void mesh_smoke_invalid_scenes();
  void mesh_smoke_captures();
  void mesh_smoke_finish(const render::ViewportStats &s);
  void smoke_rejection(QString name, const std::function<void()> &operation);
  std::size_t profile_phase(std::uint64_t frame) const;
  QString profile_phase_name(std::uint64_t frame) const;
  bool profile_ready_for_frame();
  bool eventFilter(QObject *watched, QEvent *event) override;
  void collect_profile_gpu(const render::ViewportStats &stats);
  void profile_interaction();
  void profile_sample(double interval_ms, double cpu_ms);
  // profile_sample part: appends the frame's sample row.
  void append_profile_sample(double interval_ms, double cpu_ms, const render::ViewportStats &stats,
                             const desktop::profile::AllocationSnapshot &allocations);
  void finish_profile(bool capture_final);
  // finish_profile part: the editor-profile.json receipt.
  QJsonObject profile_receipt(const render::ViewportStats &stats, const QJsonArray &gpu_samples,
                              const QJsonArray &camera, const QJsonObject &actual_phases,
                              std::uint64_t expected_frames, bool workload_verified, bool gpu_complete,
                              const desktop::profile::AllocationSnapshot &allocations, bool passed) const;
  void file_profile_completed(desktop::PersistenceKind kind, double wait_ms, double publish_ms,
                              double snapshot_ms, double worker_ms);
  void file_profile_interaction();
  void finish_file_profile(bool capture_final);
  // finish_file_profile part: writes final.scene.json from the current Scene.
  void write_file_profile_scene();
  void smoke_step();
  // smoke_step parts, called in this order on every frame.
  void smoke_authoring_frames();
  void smoke_play_and_file_frames();
  void smoke_background_stages();
  void smoke_pending_text_stages();
  void smoke_preview_stages();
  void smoke_intermediate_input_rejections();
  void smoke_play_commit_and_checkpoint();
  void smoke_checkpoint_stages();
  void smoke_recovery_stages();
  void smoke_finish();
  void smoke_type(const QString &text);
  void closeEvent(QCloseEvent *event) override;
};
} // namespace race_editor
