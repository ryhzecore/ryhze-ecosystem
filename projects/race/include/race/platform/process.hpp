#pragma once
#include "race/platform/platform.hpp"
#include <optional>

namespace race::platform::process {
struct Request {
  std::uint64_t service{},serial{};
  friend bool operator==(const Request&,const Request&)=default;
};
struct Limits {
  std::size_t stdout_bytes{65536},stderr_bytes{16384};
  std::uint64_t requests{~std::uint64_t{0}};
};
struct Launch {
  std::string_view executable,working_directory;
  std::span<const std::string_view> arguments;
};
enum class Phase : std::uint8_t { Running,Stopping,Completed };
struct Snapshot {
  Request request;
  Phase phase{Phase::Running};
  std::uint64_t stdout_observed{},stderr_observed{};
  std::size_t stdout_retained{},stderr_retained{};
  bool stdout_finished{},stderr_finished{};
  std::optional<std::uint32_t> exit_code;
  std::optional<Error> failure;
};
struct Completion {
  Snapshot status;
  std::vector<std::byte> out,err;
};
class Service {
public:
  virtual ~Service()=default;
  // One slot, including untaken completion. Launch data is copied before start.
  virtual Result<Request> start(const Launch&)=0;
  // Fixed work; no deliberate wait. Returned spans last until take/destruction.
  virtual Result<Snapshot> poll(Request) noexcept=0;
  virtual Result<std::span<const std::byte>> stdout_prefix(Request) const noexcept=0;
  virtual Result<std::span<const std::byte>> stderr_prefix(Request) const noexcept=0;
  virtual Result<void> request_stop(Request) noexcept=0;
  // Completion-stage timeout: it does not stop the child or release ownership.
  // At most 60,000 ms per call; zero performs one poll. Polling drains both pipes.
  virtual Result<Snapshot> wait(Request,std::uint32_t milliseconds) noexcept=0;
  virtual Result<Completion> take(Request) noexcept=0;
};
// Owner-thread methods. The composition supplies a never-reused nonzero identity.
// Destruction requests termination and waits for process AND stream quiescence;
// it has no timeout that could detach a child. No credentials or caller token changes.
// First Windows trusted-tool profile: one process, 256 MiB committed memory,
// 16 arguments excluding argv[0], 16 KiB encoded command line including its NUL,
// bounded inherited stdout/stderr. The child has no console; redirected handles
// are explicit. Console-free creation does not relinquish service ownership.
// A minimal child environment is used. This is not filesystem/network isolation.
Result<std::unique_ptr<Service>> make_windows_service(std::uint64_t,const Limits& = {});
} // namespace race::platform::process
