#pragma once
#include "race/core/jobs.hpp"
#include "race/core/memory.hpp"
#include "race/core/diagnostics.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace race::runtime {
enum Service : std::uint32_t { Memory = 1, Jobs = 2, Clock = 4, Diagnostics = 8 };
struct ModuleDescriptor {
  std::string id;
  std::uint32_t contract_version = 1;
  std::vector<std::string> dependencies;
  std::uint64_t required_capabilities = 0;
  std::uint32_t services = 0;
};
// Explicit borrows: undeclared services are null, never looked up globally.
struct ModuleContext {
  TrackedAllocator* memory = nullptr;
  core::Scheduler* jobs = nullptr;
  platform::Clock* clock = nullptr;
  core::Diagnostics* diagnostics = nullptr;
  core::GroupHandle task_group;
};
class Module {
public:
  virtual ~Module() = default;
  virtual const ModuleDescriptor& descriptor() const noexcept = 0;
  virtual Result<void> validate() noexcept = 0;
  virtual Result<void> initialize(ModuleContext) noexcept = 0;
  virtual Result<void> start() noexcept = 0;
  virtual void request_stop() noexcept = 0;
  // Proves external consumers (e.g. dedicated I/O) stopped; CPU group is already
  // quiescent when invoked. Timeout/Busy retains all resources and dependencies.
  virtual Result<void> quiesce(std::chrono::milliseconds timeout) noexcept = 0;
  virtual void shutdown() noexcept = 0;
  // Called for a failed initialize, after quiescence; never shutdown on an
  // uninitialized module. Must clean partially created resources.
  virtual void rollback_initialization() noexcept = 0;
};
enum class ModuleState { Registered, Validated, Initializing, Initialized, Running, Stopping, Quiescent, Stopped };
class Runtime {
  struct Entry {
    std::unique_ptr<Module> module;
    ModuleDescriptor descriptor;
    core::GroupHandle group;
    ModuleState state = ModuleState::Registered;
    bool group_created = false, initialized = false, stop_requested = false;
  };
  TrackedAllocator& memory_;
  core::Scheduler& jobs_;
  platform::Clock& clock_;
  core::Diagnostics& diagnostics_;
  std::uint64_t capabilities_;
  std::thread::id owner_ = std::this_thread::get_id();
  std::vector<Entry> entries_;
  std::vector<std::size_t> order_;
  bool attempted_ = false, running_ = false;
  Result<void> validate_graph();
public:
  Runtime(TrackedAllocator&, core::Scheduler&, platform::Clock&, core::Diagnostics&, std::uint64_t capabilities = 0);
  ~Runtime(); // Safely waits indefinitely if a consumer refuses to quiesce.
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Result<void> add(std::unique_ptr<Module>);
  Result<void> start();
  Result<void> stop(std::chrono::milliseconds timeout);
  bool running() const noexcept { return running_; } // Owner-thread observation.
  Result<ModuleState> state(std::string_view id) const;
};
}
