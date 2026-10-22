#pragma once
// The editor's D3D12 Scene viewport window: navigation, picking and gizmo dragging.
#include "editor_common.hpp"

namespace race_editor {
class Viewport final : public QWindow {
  desktop::NavigationInput navigation_;
  std::uint64_t navigation_update_{};
  bool navigation_enabled_{true};
  static std::optional<desktop::NavigationAction> movement(int key);
  desktop::Document &document_;
  const std::shared_ptr<const desktop::MeshSet> &meshes_;
  std::filesystem::path shaders_;
  QPointF last_, start_;
  std::optional<desktop::ObjectRow> preview_;
  desktop::ObjectRow before_;
  std::uint64_t drag_generation_{};
  bool orbiting_{}, dragging_{};
  std::optional<desktop::gizmo::Layout> drag_layout_, displayed_gizmo_;
  std::array<Vec, 3> move_axes_{};
  std::array<double, 3> axis_lengths_{};
  double rotation_previous_{}, rotation_total_{};
  double yaw_{.65}, pitch_{.42}, distance_{9};
  Vec target_{0, .5, 0};
  std::unique_ptr<render::WindowsViewport> renderer_;
  bool surface_available_{}, renderer_blocked_{};
  std::vector<render::OverlayDraw> overlay_scratch_; // Owned CPU geometry; no retained Scene/mesh leases.
public:
  PersistentId selected;
  std::uint64_t simulation_tick{};
  double extraction_ms{}, submission_ms{};
  int tool{}, axis{-1}; // -1 Select, 0 Move, 1 Rotate, 2 Scale; handles choose X/Y/Z.
  std::function<void(int)> on_tool;
  std::function<void(int)> on_axis;
  std::function<void(PersistentId)> on_select;
  std::function<bool()> on_admit_edit;
  std::function<void()> on_edit;
  std::function<void(QString)> on_error;
  std::function<void()> on_recovery_boundary;
  bool automatic_recovery_enabled{true}; // Profiling must fail on any lost/replaced epoch.
  explicit Viewport(desktop::Document &doc, const std::shared_ptr<const desktop::MeshSet> &meshes,
                    std::filesystem::path shaders);
  ~Viewport() override;
  render::WindowsViewport *renderer() const;
  bool previewing() const;
  std::array<double, 6> camera_pose() const;
  void cancel();
  void reset_navigation(bool active);
  void set_navigation_enabled(bool enabled);
  void navigate(double seconds);
  bool smoke_navigation();
  bool smoke_drag(bool cancel_edit, bool hold = false, int wanted_axis = -1);
  bool smoke_noop_drag();
  bool smoke_rotation_turns();
  bool smoke_axis_view(int view, int mode, const std::function<void()> &refresh,
                       const std::filesystem::path &image);
  bool smoke_tool_input();
  void frame_selected();
  // Recover the owned parent copy without an ill-conditioned full-TRS inverse.
  // Division removes only authored scale; quaternion transpose removes rotation.
  Result<scene::Affine> gizmo_parent(const desktop::ObjectRow &row) const;
  Result<desktop::gizmo::Layout> make_gizmo(const desktop::ObjectRow &row);
  void begin_gizmo(const desktop::ObjectRow &row, const desktop::gizmo::Layout &handles, int chosen);
  Vec eye() const;
  void frame_all();
  void report(Result<void> r);
  Result<void> recover_failed_operation(Result<void> failed);
  bool render();

protected:
  bool event(QEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void keyReleaseEvent(QKeyEvent *e) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;

private:
  bool smoke_drag_endpoints(const desktop::ObjectRow &row, int wanted_axis, QPointF &start, QPointF &end);

  void smoke_selection_only_input(bool &passed, std::uint64_t generation);

  bool prepare_render_surface();

  void pick_geometry(QMouseEvent *e, const std::vector<desktop::ObjectRow> &rows);
};
} // namespace race_editor
