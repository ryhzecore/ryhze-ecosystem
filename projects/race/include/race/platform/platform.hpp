#pragma once
#include "race/foundation/result.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace race::platform {
struct MonotonicTime { std::uint64_t nanoseconds; std::uint64_t domain; };
class Clock {
public:
  virtual ~Clock() = default;
  virtual MonotonicTime now() const noexcept = 0;
};
class Thread {
public:
  virtual ~Thread() = default; // Implementations join; never detach.
  virtual Result<void> join() noexcept = 0; // Serialized by owner, not concurrent join callers.
};
class Threads {
public:
  virtual ~Threads() = default;
  virtual Result<std::unique_ptr<Thread>> start(std::function<void()> body) = 0;
};
enum class Durability { AtomicVisibility, Durable };
class Files {
public:
  virtual ~Files() = default;
  virtual Result<std::vector<std::byte>> read(std::string_view path, std::size_t limit) = 0;
  // Staging is private and same-directory; failure before rename preserves target.
  // Durable is explicit Unsupported in M1 (atomic visibility != power-loss durability).
  virtual Result<void> replace(std::string_view path, std::span<const std::byte> bytes, Durability) = 0;
};
struct MemoryInfo { std::uint64_t physical_bytes; std::uint64_t resident_bytes; std::uint64_t peak_resident_bytes{0}; };
struct Services {
  std::unique_ptr<Clock> clock;
  std::unique_ptr<Threads> threads;
  std::unique_ptr<Files> files;
};
// Factory has no OS types; concrete backend linked by the composition executable only.
Result<Services> make_linux_services();
Result<MemoryInfo> linux_memory_info();
Result<Services> make_windows_services();
Result<MemoryInfo> windows_memory_info();
inline Result<Services> make_services() {
#ifdef _WIN32
  return make_windows_services();
#else
  return make_linux_services();
#endif
}
inline Result<MemoryInfo> memory_info() {
#ifdef _WIN32
  return windows_memory_info();
#else
  return linux_memory_info();
#endif
}
void emergency_write(std::string_view message) noexcept;
}
