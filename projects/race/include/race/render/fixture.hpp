#pragma once
#include "race/foundation/identity.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace race::render {
// Phase-one fixture contract, not a renderer/media/camera API. Coordinates are
// metres, right handed Y-up; the test projection maps origin-relative XY to NDC.
// Scene objects have World-local IDs: composition supplies a distinct world_scope.
struct SceneSnapshot {
  PersistentId world_scope, object, resource;
  std::uint64_t generation{}, origin_revision{};
  std::array<std::byte, 32> resource_revision{};
  std::array<double, 3> origin{};
  std::array<double, 12> world_from_local{1,0,0,0,0,1,0,0,0,0,1,0};
  std::array<std::array<double, 3>, 3> vertices{{{-.7,-.7,0},{.7,-.7,0},{0,.7,0}}};
  std::array<float, 4> color{.25f,.625f,.9375f,1};
};
enum class Execution { CpuContract, SoftwareGraphics };
struct Capabilities {
  Execution execution{Execution::CpuContract};
  std::uint32_t max_width{512}, max_height{512};
  std::size_t retained_bytes{1024 * 1024};
  bool completion_fence{false};
  // No hardware, ray tracing, importers, HDR display or streaming capability.
};
struct Frame {
  SceneSnapshot scene;
  std::uint32_t width{}, height{};
  std::array<std::array<float, 4>, 3> positions{};
};
Result<Frame> prepare(SceneSnapshot, std::uint32_t width, std::uint32_t height,
                      Capabilities) noexcept;
// Strong opaque owner supplied by executable composition (e.g. an Assets Lease).
// bytes counts retained resource/dependency PAYLOAD, not a handle or driver memory.
// Composition separately bounds metadata (one Lease wrapper for this fixture).
struct ResourceOwner { std::shared_ptr<const void> owner; std::size_t bytes{}; };
struct Ticket {
  std::uint64_t queue{}, serial{};
  friend bool operator==(const Ticket &, const Ticket &) = default;
};
enum class Terminal { Complete, Failed, Cancelled };
// Exactly one resident fixture. submit validates before publication. Cancellation
// and wait timeout never release ownership or imply device completion. Backend
// must acknowledge actual quiescence with the matching evidence category. This is
// a trusted composition contract, not proof that a caller used a graphics API.
class Flight {
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::uint64_t identity_{}, serial_{};
  bool occupied_{}, pending_{}, cancelled_{};
  Terminal terminal_{Terminal::Failed};
  Frame frame_{};
  ResourceOwner resource_{};
  Execution execution_{};
  bool matches(Ticket) const noexcept;
public:
  Flight();
  // Backend must remain able to acknowledge while destruction blocks. Destroying
  // on the backend's own completion thread before acknowledgement would deadlock.
  ~Flight();
  Flight(const Flight &) = delete;
  Flight &operator=(const Flight &) = delete;
  Result<Ticket> submit(SceneSnapshot, std::uint32_t, std::uint32_t,
                        Capabilities, ResourceOwner);
  Result<Frame> frame(Ticket) const;
  Result<void> cancel(Ticket);
  Result<void> acknowledge_quiescence(Ticket, Execution evidence, Terminal);
  Result<Terminal> wait(Ticket, std::chrono::milliseconds);
  Result<Terminal> take(Ticket);
};
} // namespace race::render
