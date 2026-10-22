#include "race/runtime/lifecycle.hpp"
#include <algorithm>
#include <functional>
#include <new>

namespace race::runtime {
Runtime::Runtime(TrackedAllocator& m, core::Scheduler& j, platform::Clock& c, core::Diagnostics& d, std::uint64_t capabilities)
 : memory_(m), jobs_(j), clock_(c), diagnostics_(d), capabilities_(capabilities) { entries_.reserve(32); order_.reserve(32); }
Runtime::~Runtime() {
  for (;;) {
    auto stopped = stop(std::chrono::seconds(1));
    if (stopped) break;
    if (stopped.error().code != ErrorCode::Timeout && stopped.error().code != ErrorCode::Busy)
      core::invariant_failure("runtime destruction cannot prove quiescence");
  }
}
Result<void> Runtime::add(std::unique_ptr<Module> module) {
  if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread, "runtime owner thread required");
  if (attempted_) return fail(ErrorCode::Stopping, "registry is frozen");
  if (!module) return fail(ErrorCode::InvalidInput, "null module");
  if (entries_.size() == 32) return fail(ErrorCode::QueueFull, "module registry full");
  const auto& desc = module->descriptor();
  if (desc.id.empty() || desc.id.size() > 64 || desc.dependencies.size() > 16 || (desc.services & ~15U))
    return fail(ErrorCode::InvalidInput, "invalid module descriptor");
  for (const auto& e : entries_) if (e.descriptor.id == desc.id) return fail(ErrorCode::Duplicate, "duplicate module id");
  try { Entry entry; entry.descriptor = desc; entry.module = std::move(module); entries_.push_back(std::move(entry)); return {}; }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "module descriptor allocation"); }
}
Result<void> Runtime::validate_graph() {
  std::array<unsigned char,32> colors{};
  auto visit = [&](auto&& self, std::size_t index) -> Result<void> {
    if (colors[index] == 1) return fail(ErrorCode::Cycle, "module dependency cycle");
    if (colors[index] == 2) return {};
    colors[index] = 1;
    const auto& desc = entries_[index].descriptor;
    if (desc.contract_version != 1) return fail(ErrorCode::IncompatibleVersion, "module contract version");
    if ((desc.required_capabilities & capabilities_) != desc.required_capabilities)
      return fail(ErrorCode::Unsupported, "required module capability unavailable");
    for (const auto& dependency : desc.dependencies) {
      auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.descriptor.id == dependency; });
      if (it == entries_.end()) return fail(ErrorCode::InvalidDependency, "missing module dependency");
      auto result = self(self, static_cast<std::size_t>(it - entries_.begin())); if (!result) return result;
    }
    colors[index] = 2; order_.push_back(index); return {};
  };
  order_.clear();
  for (std::size_t i = 0; i < entries_.size(); ++i) { auto result = visit(visit, i); if (!result) { order_.clear(); return result; } }
  for (auto index : order_) {
    auto result = entries_[index].module->validate(); if (!result) return result;
    entries_[index].state = ModuleState::Validated;
  }
  return {};
}
Result<void> Runtime::start() {
  if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread, "runtime owner thread required");
  if (attempted_) return fail(ErrorCode::InvalidInput, "runtime starts once");
  attempted_ = true;
  auto validated = validate_graph(); if (!validated) return validated;
  auto unwind = [&](Error error) -> Result<void> {
    auto stopped = stop(std::chrono::seconds(5));
    if (!stopped) { auto log = diagnostics_.log(core::LogLevel::Error, "startup rollback waiting for quiescence"); (void)log; }
    return error;
  };
  for (auto index : order_) {
    auto& entry = entries_[index];
    auto group = jobs_.create_group(); if (!group) return unwind(group.error());
    entry.group = group.value(); entry.group_created = true; entry.state = ModuleState::Initializing;
    auto services = entry.descriptor.services;
    ModuleContext context{services & Memory ? &memory_ : nullptr, services & Jobs ? &jobs_ : nullptr,
                          services & Clock ? &clock_ : nullptr, services & Diagnostics ? &diagnostics_ : nullptr, entry.group};
    auto initialized = entry.module->initialize(context); if (!initialized) return unwind(initialized.error());
    entry.initialized = true; entry.state = ModuleState::Initialized;
    auto started = entry.module->start(); if (!started) return unwind(started.error());
    entry.state = ModuleState::Running;
  }
  running_ = true; return {};
}
Result<void> Runtime::stop(std::chrono::milliseconds timeout) {
  if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread, "runtime owner thread required");
  if (timeout.count() < 0) return fail(ErrorCode::InvalidInput, "negative shutdown timeout");
  const auto begin = std::chrono::steady_clock::now();
  auto remaining = [&] {
    if (timeout == std::chrono::milliseconds::max()) return timeout;
    auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    return spent >= timeout ? std::chrono::milliseconds(0) : timeout - spent;
  };
  running_ = false;
  for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
    auto& entry = entries_[*it];
    if (!entry.group_created || entry.state == ModuleState::Stopped) continue;
    if (!entry.stop_requested) {
      entry.stop_requested = true; entry.state = ModuleState::Stopping;
      entry.module->request_stop();
      auto closed = jobs_.close(entry.group); if (!closed) return closed;
      auto cancelled = jobs_.cancel(entry.group); if (!cancelled) return cancelled;
    }
    auto work = jobs_.wait(entry.group, remaining()); if (!work) return work.error();
    auto quiescent = entry.module->quiesce(remaining()); if (!quiescent) return quiescent;
    entry.state = ModuleState::Quiescent;
    // Completion payloads are released before module resources only after every
    // job is terminal; reverse dependency ordering protects producers.
    auto released = jobs_.release(entry.group); if (!released) return released;
    entry.group_created = false;
    if (entry.initialized) entry.module->shutdown(); else entry.module->rollback_initialization();
    entry.state = ModuleState::Stopped;
  }
  return {};
}
Result<ModuleState> Runtime::state(std::string_view id) const {
  if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread, "runtime owner thread required");
  for (const auto& e : entries_) if (e.descriptor.id == id) return e.state;
  return fail(ErrorCode::NotFound, "unknown module");
}
}
