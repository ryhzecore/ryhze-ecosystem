#pragma once
#include "race/platform/platform.hpp"
#include <limits>
#include <optional>

namespace race::platform::input {
inline constexpr std::size_t device_capacity=16, batch_capacity=128, work_capacity=1024;
enum class DeviceKind : std::uint8_t { Keyboard, Mouse };
// Registered means an identified instance, which may be a virtual driver.
enum class Provenance : std::uint8_t { Registered, Unknown, Touch, Pen };
// Physical positions in this first camera-input profile, not gameplay actions.
enum class Key : std::uint8_t { None, A, D, W, S, Q, E, Escape };
enum Modifier : std::uint8_t { Shift=1, Control=2, Alt=4, Gui=8 };
enum class Kind : std::uint8_t {
  Ignored, Key, RightButton, Motion, Wheel, FocusLost, FocusGained,
  Minimized, Restored, Resized, Close, Quit, DeviceAdded, DeviceRemoved
};
struct Registration { DeviceKind kind{}; std::uint64_t instance{}; };
// Backend-normalized value. Nonzero instance tokens must never be reused for a
// different device within a Source lifetime (including after removal). Tokens
// are private to this source's lifetime;
// consumers receive bounded slot/generation handles instead. No borrowed payloads.
struct SourceEvent {
  Kind kind{Kind::Ignored};
  DeviceKind device_kind{};
  Provenance provenance{Provenance::Unknown};
  std::uint64_t instance{};
  std::optional<MonotonicTime> source_time;
  Key key{Key::None};
  std::uint8_t modifiers{};
  bool pressed{}, repeat{};
  double x{},y{}; // Relative motion in window pixels, wheel in normalized steps.
};
class Source {
public:
  virtual ~Source()=default;
  virtual std::span<const Registration> initial_devices() const noexcept=0;
  // Owner-thread, synchronous with no intentional wait. Native polling may stall
  // (e.g. modal OS window operations); bounds are records, not wall-clock time.
  // One examined record (possibly Ignored), or no record at this poll boundary.
  // No callbacks or retained asynchronous I/O.
  virtual Result<std::optional<SourceEvent>> next() noexcept=0;
};
struct Device {
  DeviceKind kind{};
  Provenance provenance{Provenance::Unknown};
  std::uint32_t slot{}; // Meaningful only for Registered provenance.
  std::uint64_t generation{};
  friend bool operator==(const Device&,const Device&)=default;
};
struct Event {
  Kind kind{};
  std::uint64_t service{}, epoch{}, sequence{};
  MonotonicTime delivered{};
  std::optional<MonotonicTime> source_time;
  Device device;
  Key key{Key::None};
  std::uint8_t modifiers{};
  bool pressed{},repeat{};
  double x{},y{};
};
struct Limits {
  std::size_t devices{device_capacity};
  // Narrowable finite counter ceilings; no wrap or implicit service restart.
  std::uint64_t sequences{std::numeric_limits<std::uint64_t>::max()},
      epochs{std::numeric_limits<std::uint64_t>::max()},
      generations{std::numeric_limits<std::uint64_t>::max()};
};
struct Batch {
  std::size_t written{},examined{},ignored{},stale_devices{};
  // Limit reached does not assert that another native event exists.
  bool output_full{},work_limit_reached{},observed_empty{};
  // A terminal failure preserves the delivered prefix and exact consumption
  // counts. The consumer must cancel held actions even if it discards the prefix.
  std::optional<Error> terminal;
};
struct Status {
  std::uint64_t service{},epoch{},sequence{};
  std::size_t devices{};
  bool closed{};
  std::optional<Error> terminal;
};
class Events {
public:
  virtual ~Events()=default;
  virtual Result<Batch> poll(std::span<Event>,std::size_t work_limit=work_capacity) noexcept=0;
  virtual Result<Status> status() const noexcept=0;
  virtual Result<void> close() noexcept=0;
};
// Clock outlives the service; source ownership transfers. The composition owner
// supplies a nonzero, never-reused service identity for this application session.
// Initialization may allocate. Engine polling/translation/close do not allocate.
// Methods are owner-thread only; close is idempotent, admission stops immediately,
// and destruction follows synchronous quiescence. Source/OS internals are separate.
Result<std::unique_ptr<Events>> make_events(std::unique_ptr<Source>,Clock&,
    std::uint64_t service,const Limits& = {});
} // namespace race::platform::input
