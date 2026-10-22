#include "race/render/fixture.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace race::render {
namespace {
std::atomic<std::uint64_t> next_queue{1};
bool finite(double v) { return std::isfinite(v) && std::abs(v) <= 1e6; }
}
Result<Frame> prepare(SceneSnapshot s, std::uint32_t w, std::uint32_t h,
                      Capabilities caps) noexcept {
  if (caps.execution != Execution::CpuContract && caps.execution != Execution::SoftwareGraphics)
    return fail(ErrorCode::Unsupported, "unknown execution category");
  if (caps.execution == Execution::SoftwareGraphics && !caps.completion_fence)
    return fail(ErrorCode::Unsupported, "software graphics requires completion fence");
  if (w < 16 || h < 16 || w > 512 || h > 512 || w > caps.max_width || h > caps.max_height)
    return fail(ErrorCode::Exhausted, "fixture target dimensions exceed bounded capability");
  if (!s.world_scope.valid() || !s.object.valid() || !s.resource.valid() || !s.generation ||
      !s.origin_revision || std::all_of(s.resource_revision.begin(), s.resource_revision.end(),
                                      [](std::byte b){ return b == std::byte{}; }))
    return fail(ErrorCode::InvalidInput, "fixture identity incomplete");
  for (double x : s.origin) if (!finite(x)) return fail(ErrorCode::InvalidInput, "nonfinite origin");
  for (double x : s.world_from_local) if (!finite(x)) return fail(ErrorCode::InvalidInput, "nonfinite transform");
  for (float x : s.color) if (!std::isfinite(x) || x < 0 || x > 1)
    return fail(ErrorCode::InvalidInput, "fixture requires UNORM color");
  if (s.color[3] != 1) return fail(ErrorCode::Unsupported, "only opaque fixture supported");
  Frame f{s,w,h,{}};
  for (std::size_t i=0;i<3;++i) {
    for (double x : s.vertices[i]) if (!finite(x)) return fail(ErrorCode::InvalidInput, "invalid vertex");
    std::array<double,3> p{};
    for (std::size_t r=0;r<3;++r) {
      p[r]=s.world_from_local[r*4+3]-s.origin[r];
      for (std::size_t c=0;c<3;++c) p[r]+=s.world_from_local[r*4+c]*s.vertices[i][c];
      if (!finite(p[r]) || std::abs(p[r])>1) return fail(ErrorCode::Unsupported, "outside bounded fixture projection");
    }
    if (p[2] != 0) return fail(ErrorCode::Unsupported, "fixture projection requires z zero");
    // Vulkan viewport Y is down. Projection flip is explicit, not a Scene change.
    f.positions[i]={float(p[0]),float(-p[1]),0,1};
  }
  const auto &a=f.positions[0], &b=f.positions[1], &c=f.positions[2];
  if (std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))<.01f)
    return fail(ErrorCode::InvalidInput, "degenerate triangle");
  return f;
}
Flight::Flight() {
  auto value=next_queue.load();
  while (value != std::numeric_limits<std::uint64_t>::max()) {
    if (next_queue.compare_exchange_weak(value,value+1)) { identity_=value; break; }
  }
}
Flight::~Flight() {
  std::unique_lock lock(mutex_);
  changed_.wait(lock,[this]{return !pending_;});
}
bool Flight::matches(Ticket t) const noexcept {
  return occupied_ && t.queue==identity_ && t.serial==serial_;
}
Result<Ticket> Flight::submit(SceneSnapshot s, std::uint32_t w, std::uint32_t h,
                             Capabilities caps, ResourceOwner owner) {
  auto f=prepare(std::move(s),w,h,caps);
  if (!f) return f.error();
  if (!owner.owner || !owner.bytes) return fail(ErrorCode::InvalidInput, "missing retained owner");
  if (owner.bytes > caps.retained_bytes || owner.bytes > 1024*1024)
    return fail(ErrorCode::Exhausted, "retained fixture exceeds byte cap");
  std::lock_guard lock(mutex_);
  if (occupied_) return fail(ErrorCode::Busy, "one fixture already resident");
  if (!identity_ || serial_==std::numeric_limits<std::uint64_t>::max())
    return fail(ErrorCode::Exhausted, "fixture ticket space retired");
  frame_=f.value();resource_=std::move(owner);execution_=caps.execution;
  occupied_=pending_=true;cancelled_=false;++serial_;
  return Ticket{identity_,serial_};
}
Result<Frame> Flight::frame(Ticket t) const {
  std::lock_guard lock(mutex_);
  if (!matches(t)) return fail(ErrorCode::StaleHandle, "foreign or stale fixture ticket");
  return frame_;
}
Result<void> Flight::cancel(Ticket t) {
  std::lock_guard lock(mutex_);
  if (!matches(t)) return fail(ErrorCode::StaleHandle, "foreign or stale fixture ticket");
  if (!pending_) return fail(ErrorCode::Busy, "fixture already terminal");
  cancelled_=true;return {};
}
Result<void> Flight::acknowledge_quiescence(Ticket t, Execution evidence, Terminal outcome) {
  std::lock_guard lock(mutex_);
  if (!matches(t)) return fail(ErrorCode::StaleHandle, "foreign or stale fixture ticket");
  if (!pending_) return fail(ErrorCode::Busy, "duplicate terminal acknowledgement");
  if (evidence!=execution_) return fail(ErrorCode::InvalidInput, "completion evidence category mismatch");
  if (outcome!=Terminal::Complete && outcome!=Terminal::Failed && outcome!=Terminal::Cancelled)
    return fail(ErrorCode::InvalidInput, "invalid terminal outcome");
  pending_=false;terminal_=outcome==Terminal::Failed?Terminal::Failed:(cancelled_?Terminal::Cancelled:outcome);
  changed_.notify_all();return {};
}
Result<Terminal> Flight::wait(Ticket t, std::chrono::milliseconds timeout) {
  if (timeout < std::chrono::milliseconds::zero())
    return fail(ErrorCode::InvalidInput, "negative completion wait");
  std::unique_lock lock(mutex_);
  if (!matches(t)) return fail(ErrorCode::StaleHandle, "foreign or stale fixture ticket");
  const auto ready=[&]{return !matches(t) || !pending_;};
  if (timeout==std::chrono::milliseconds::max()) changed_.wait(lock,ready);
  else if (!changed_.wait_for(lock,timeout,ready))
    return fail(ErrorCode::Timeout, "completion wait expired; owner retained");
  if (!matches(t)) return fail(ErrorCode::StaleHandle, "fixture taken during wait");
  return terminal_;
}
Result<Terminal> Flight::take(Ticket t) {
  // Drop arbitrary user ownership after unlocking, preventing destructor reentry.
  ResourceOwner released;
  Terminal result;
  {
    std::lock_guard lock(mutex_);
    if (!matches(t)) return fail(ErrorCode::StaleHandle, "foreign or stale fixture ticket");
    if (pending_) return fail(ErrorCode::Busy, "fixture still in flight");
    result=terminal_;released=std::move(resource_);occupied_=false;changed_.notify_all();
  }
  return result;
}
} // namespace race::render
