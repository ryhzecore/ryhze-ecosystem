#include "race/platform/platform.hpp"
#include "race/platform/linux_test_hooks.hpp"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <thread>
#include <time.h>
#include <unistd.h>

namespace race::platform {
void emergency_write(std::string_view message) noexcept {
  while (!message.empty()) {
    auto written = ::write(STDERR_FILENO, message.data(), message.size());
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return;
    message.remove_prefix(static_cast<std::size_t>(written));
  }
}
namespace {
struct Fd {
  int value;
  explicit Fd(int fd) : value(fd) {}
  ~Fd() { if (value >= 0) ::close(value); }
};
bool valid_path(std::string_view path) {
  return !path.empty() && path.front() == '/' && path.size() < PATH_MAX &&
         path.find('\0') == std::string_view::npos;
}
class LinuxClock final : public Clock {
  MonotonicTime now() const noexcept override {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) std::abort();
    return {static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(value.tv_nsec), 1};
  }
};
class LinuxThread final : public Thread {
  std::thread thread_;
public:
  explicit LinuxThread(std::function<void()> body) : thread_(std::move(body)) {}
  ~LinuxThread() override {
    if (thread_.joinable()) {
      if (thread_.get_id() == std::this_thread::get_id()) std::abort();
      thread_.join();
    }
  }
  Result<void> join() noexcept override {
    if (!thread_.joinable()) return {};
    if (thread_.get_id() == std::this_thread::get_id())
      return fail(ErrorCode::WrongThread, "cannot join own thread");
    try { thread_.join(); return {}; }
    catch (...) { return fail(ErrorCode::PlatformFailure, "thread join failed"); }
  }
};
class LinuxThreads final : public Threads {
  Result<std::unique_ptr<Thread>> start(std::function<void()> body) override {
    if (!body) return fail(ErrorCode::InvalidInput, "empty thread body");
    try { return std::unique_ptr<Thread>(new LinuxThread(std::move(body))); }
    catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "thread owner allocation"); }
    catch (...) { return fail(ErrorCode::PlatformFailure, "thread start failed"); }
  }
};
class LinuxFiles final : public Files {
  const std::size_t write_failure_after_;
  const std::optional<LinuxFileFault> fault_;
public:
  explicit LinuxFiles(std::size_t failure_after = std::numeric_limits<std::size_t>::max(),
      std::optional<LinuxFileFault> fault = std::nullopt)
    : write_failure_after_(failure_after), fault_(fault) {}
private:
  Result<std::vector<std::byte>> read(std::string_view path, std::size_t limit) override {
    if (!valid_path(path) || limit == 0) return fail(ErrorCode::InvalidInput, "invalid file request");
    try {
      std::string name(path);
      Fd fd(::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
      if (fd.value < 0) return fail(ErrorCode::IoFailure, "file open failed");
      struct stat info{};
      if (::fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return fail(ErrorCode::InvalidInput, "regular file required");
      if (static_cast<std::uint64_t>(info.st_size) > limit)
        return fail(ErrorCode::Exhausted, "file exceeds read limit");
      std::vector<std::byte> bytes;
      bytes.reserve(static_cast<std::size_t>(info.st_size));
      std::byte buffer[4096];
      for (;;) {
        const auto count = ::read(fd.value, buffer, sizeof buffer);
        if (count < 0) {
          if (errno == EINTR) continue;
          return fail(ErrorCode::IoFailure, "file read failed");
        }
        if (count == 0) return bytes;
        auto n = static_cast<std::size_t>(count);
        if (n > limit - bytes.size()) return fail(ErrorCode::Exhausted, "file grew past read limit");
        bytes.insert(bytes.end(), buffer, buffer + n);
      }
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "file read allocation"); }
      catch (const std::length_error&) { return fail(ErrorCode::Exhausted, "file read capacity"); }
  }
  Result<void> replace(std::string_view path, std::span<const std::byte> bytes, Durability durability) override {
    if (!valid_path(path)) return fail(ErrorCode::InvalidInput, "invalid replacement path");
    if (durability == Durability::Durable) return fail(ErrorCode::Unsupported, "durable replacement unavailable in M1");
    if (durability != Durability::AtomicVisibility) return fail(ErrorCode::InvalidInput, "invalid durability policy");
    try {
      std::string target(path);
      struct stat old{};
      if (::lstat(target.c_str(), &old) == 0 && !S_ISREG(old.st_mode))
        return fail(ErrorCode::InvalidInput, "replacement target must be regular");
      auto slash = target.rfind('/');
      std::string staging = target.substr(0, slash + 1) + ".race-stage-XXXXXX";
      Fd fd(::mkstemp(staging.data()));
      if (fd.value < 0) return fail(ErrorCode::IoFailure, "staging creation failed");
      struct RemoveStage { const char* path; ~RemoveStage() { ::unlink(path); } } cleanup{staging.c_str()};
      std::size_t done = 0;
      bool interrupted = false;
      while (done < bytes.size()) {
        if (done >= write_failure_after_) return fail(ErrorCode::IoFailure, "injected interrupted staging write");
        ssize_t count;
        if (fault_ == LinuxFileFault::WriteInterruptedOnce && !interrupted) {
          interrupted = true; errno = EINTR; count = -1;
        } else {
          count = ::write(fd.value, bytes.data() + done,
                         std::min({bytes.size() - done, static_cast<std::size_t>(SSIZE_MAX), write_failure_after_ - done}));
        }
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail(ErrorCode::IoFailure, "staging write failed");
        done += static_cast<std::size_t>(count);
      }
      // Closing successfully detects delayed errors before the visibility commit.
      int raw = fd.value; fd.value = -1;
      const auto closed = ::close(raw);
      // Always actually close before injecting its failure. Retrying a Linux
      // close could close an unrelated reused descriptor.
      if (closed != 0 || fault_ == LinuxFileFault::Close)
        return fail(ErrorCode::IoFailure, "staging close failed");
      if (fault_ == LinuxFileFault::Rename)
        return fail(ErrorCode::IoFailure, "injected atomic replacement failure");
      if (::rename(staging.c_str(), target.c_str()) != 0)
        return fail(ErrorCode::IoFailure, "atomic replacement failed");
      return {};
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "file replacement allocation"); }
  }
};
}
Result<Services> make_linux_services() {
  try { return Services{std::make_unique<LinuxClock>(), std::make_unique<LinuxThreads>(), std::make_unique<LinuxFiles>()}; }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "platform creation allocation"); }
}
Result<std::unique_ptr<Files>> linux_files_with_write_fault(std::size_t after_bytes) {
  try { return std::unique_ptr<Files>(new LinuxFiles(after_bytes)); }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "file fixture allocation"); }
}
Result<std::unique_ptr<Files>> linux_files_with_fault(LinuxFileFault fault) {
  if (fault != LinuxFileFault::WriteInterruptedOnce && fault != LinuxFileFault::Close && fault != LinuxFileFault::Rename)
    return fail(ErrorCode::InvalidInput, "invalid native file fault fixture");
  try { return std::unique_ptr<Files>(new LinuxFiles(std::numeric_limits<std::size_t>::max(), fault)); }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "file fixture allocation"); }
}
Result<MemoryInfo> linux_memory_info() {
  struct sysinfo info{};
  if (::sysinfo(&info) != 0) return fail(ErrorCode::PlatformFailure, "memory query failed");
  auto page = ::sysconf(_SC_PAGESIZE);
  auto file = ::fopen("/proc/self/statm", "r");
  unsigned long pages = 0, resident = 0;
  if (!file || page <= 0) { if (file) ::fclose(file); return fail(ErrorCode::Unsupported, "resident memory unavailable"); }
  auto parsed = ::fscanf(file, "%lu %lu", &pages, &resident); ::fclose(file);
  if (parsed != 2) return fail(ErrorCode::PlatformFailure, "resident memory parse failed");
  return MemoryInfo{static_cast<std::uint64_t>(info.totalram) * info.mem_unit,
                    static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(page)};
}
}
