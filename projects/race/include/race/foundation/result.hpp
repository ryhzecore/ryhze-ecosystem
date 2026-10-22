#pragma once
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <variant>

namespace race {
enum class ErrorCode : std::uint8_t {
  InvalidInput, Unsupported, Exhausted, QueueFull, Cancelled, Stopping,
  IncompatibleVersion, PlatformFailure, InvalidDependency, Cycle,
  StaleHandle, Busy, Timeout, FailedPrerequisite, WrongThread, Overflow,
  NotFound, Duplicate, Invariant, IoFailure
};
// Messages are static literals, never caller-owned buffers or sensitive paths.
struct Error { ErrorCode code; const char* message; };
inline Error fail(ErrorCode code, const char* message) noexcept { return {code, message}; }
template<class T> class [[nodiscard]] Result {
  std::variant<T, Error> state_;
public:
  Result(T value) : state_(std::move(value)) {}
  Result(Error error) : state_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(state_); }
  T& value() & { if (!*this) std::abort(); return std::get<T>(state_); }
  const T& value() const & { if (!*this) std::abort(); return std::get<T>(state_); }
  T&& value() && { if (!*this) std::abort(); return std::get<T>(std::move(state_)); }
  Error error() const { if (*this) std::abort(); return std::get<Error>(state_); }
};
template<> class [[nodiscard]] Result<void> {
  bool ok_ = true;
  Error error_{ErrorCode::Invariant, "no error"};
public:
  Result() = default;
  Result(Error error) : ok_(false), error_(error) {}
  explicit operator bool() const noexcept { return ok_; }
  void value() const { if (!ok_) std::abort(); }
  Error error() const { if (ok_) std::abort(); return error_; }
};
}
