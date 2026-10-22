#pragma once
// Native composition-only adapter. Never include this header in portable modules.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "race/render/viewport.hpp"
#include <filesystem>
#include <memory>
#include <thread>

namespace race::render {
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
class WindowsViewportRemovalProbe;
#endif
class WindowsViewport {
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
  friend class WindowsViewportRemovalProbe;
#endif
  struct Impl;
  struct Program;
  struct ImageOwner;
  // Declared before Impl: admitted CPU shader bytes outlive every native device.
  std::unique_ptr<const Program> program_;
  std::shared_ptr<const ImageOwner> image_owner_;
  std::unique_ptr<Impl> impl_;
  const std::thread::id owner_{std::this_thread::get_id()};
  std::uint64_t epoch_{}, recreation_attempts_{};
  std::uint32_t removal_recovery_attempts_{};
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
  bool fail_next_initialization_{};
#endif
  ViewportState state_{ViewportState::Unavailable};
  ViewportRecreation recreation_{ViewportRecreation::Initial};
  ViewportRetirement retired_;
  std::optional<Error> lifecycle_error_;
  std::uint64_t retained_debug_errors_{}, retained_debug_warnings_{}, retained_debug_discarded_{};
  bool retained_diagnostics_incomplete_{};
  std::array<char, 512> retained_debug_message_{};
  void retain_diagnostics(ViewportStats &) noexcept;
  ViewportStats unavailable_; // Retains diagnostics when native initialization fails or a surface is absent.
  explicit WindowsViewport(std::unique_ptr<const Program>);
  Result<void> owner_check() const;
  Result<void> retire_native();
  Result<void> replace(HWND, ViewportRecreation);
  static Result<std::unique_ptr<const Program>> load_program(const std::filesystem::path &);

public:
  // An expired/different owner cannot alias a new owner at the same address.
  // Tokens retain only a weak identity, never a native resource or viewport.
  class ImageToken {
    std::weak_ptr<const ImageOwner> owner_;
    std::uint64_t epoch_{}, frame_{};
    ImageToken(const std::shared_ptr<const ImageOwner> &owner, std::uint64_t epoch, std::uint64_t frame)
        : owner_(owner), epoch_(epoch), frame_(frame) {}
    friend class WindowsViewport;

  public:
    ImageToken() = default;
    std::uint64_t device_epoch() const noexcept { return epoch_; }
    std::uint64_t frame() const noexcept { return frame_; }
  };
  static Result<std::unique_ptr<WindowsViewport>> create(HWND window,
                                                         const std::filesystem::path &shader_directory);
  ~WindowsViewport(); // Retains submitted objects until fence completion/confirmed removal.
  WindowsViewport(const WindowsViewport &) = delete;
  WindowsViewport &operator=(const WindowsViewport &) = delete;
  // All access remains serialized on the creating thread, including stats().
  // Must run before Qt destroys its HWND; keeps shader bytes and owner identity.
  Result<void> retire_surface();
  // Controlled replacement drains a live device. Cannot masquerade as removed-device recovery.
  Result<void> recreate(HWND);
  // One automatic attempt per stable owner, only after native removal was observed.
  Result<void> recover_removed(HWND);
  // Zero dimensions suspend presentation and invalidate the last image, not the device epoch.
  Result<void> resize(std::uint32_t width, std::uint32_t height);
  // Bounded opaque geometry + ground grid + selection edges, SDR sRGB.
  // Owned unlit NDC overlays draw last; invalid frames reject before submission.
  // Directional Lambert + constant ambient only; no GI/shadows/PBR/RT claims.
  Result<void> draw(const RenderFrame &);
  Result<ImageToken> image_token() const;
  // Current-image convenience retains existing callers; both paths validate a stamped token.
  Result<void> capture(const std::filesystem::path &);
  Result<void> capture(const std::filesystem::path &, const ImageToken &);
  // PhysicalDiffuseV1 only: streams the stored FP16 scene-linear pixels (unexposed
  // cd/m^2, no grid/selection/overlay annotations) as RGB32F PFM plus the tightly
  // packed raw RGBA16F rows, both digested into the returned metadata.
  Result<SceneLinearCapture> capture_scene_linear(const std::filesystem::path &pfm,
                                                  const std::filesystem::path &raw, const ImageToken &);
  ViewportState state() const noexcept; // Creating thread only; no allocations or native calls.
  ViewportStats stats() const;
};
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
// Test adapter only. Normal app builds neither declare nor link this interface.
// These are copied native observations, not a way to mutate production loss state.
struct NativeRemovalStatus {
  std::int64_t reason{}, device5_query{};
  std::uint64_t completed_fence{};
  std::uint32_t adapter_luid_low{};
  std::int32_t adapter_luid_high{};
  std::uint64_t debug_errors{}, unexpected_warnings{}, sentinel_warnings{}, messages_discarded{};
  bool diagnostics_incomplete{};
  std::uint32_t last_message_id{};
  std::array<char, 512> last_message{};
  std::uint64_t next_signal{}; // Logical test observation, distinct from native completed_fence.
  std::uint64_t nonresident_textures{};
  bool graph_plan_installed{};
};
class WindowsViewportRemovalProbe {
public:
  static Result<NativeRemovalStatus> observe(WindowsViewport &);
  static Result<void> remove_test_device(WindowsViewport &);
  static Result<void> set_next_fence(WindowsViewport &,
                                     std::uint64_t); // Quiescent, monotonic test counter seam.
  static Result<void> set_epoch(WindowsViewport &,
                                std::uint64_t); // Quiescent, monotonic; invalidates old images/timing.
  static Result<void>
  fail_next_initialization(WindowsViewport &); // Explicit injected Result failure, never simulated removal.
  static Result<void>
  reject_next_graph(WindowsViewport &, bool binding); // Corrupt only a test candidate before real validation.
  static Result<void> add_warning(WindowsViewport &);
  static Result<void> collect_diagnostics(WindowsViewport &);
  // Test-only lower ceiling for the next physical output install (0 restores the declared budget).
  // The declared ceilings themselves never change; this exercises the real Exhausted path.
  static Result<void> limit_output_budget(WindowsViewport &, std::uint64_t bytes);
};
#endif
} // namespace race::render
