#include "race/platform/rooted_files.hpp"
#include <atomic>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace race::platform {
bool valid_content_leaf(std::string_view p) noexcept {
  if (p.empty() || p.size() > 96 || p.front() == '.' || p.find("..") != p.npos)
    return false;
  for (char c : p)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
      return false;
  return true;
}
namespace {
struct Fd {
  int fd;
  ~Fd() {
    if (fd >= 0)
      ::close(fd);
  }
};
class Root final : public RootedFiles {
  Fd root_;
  std::atomic<std::uint64_t> next_{1};

public:
  explicit Root(int fd) : root_{fd} {}
  Result<std::vector<std::byte>> read(std::string_view name, std::size_t limit) override {
    if (!valid_content_leaf(name) || !limit)
      return fail(ErrorCode::InvalidInput, "invalid content leaf or limit");
    try {
      std::string n(name);
      Fd f{::openat(root_.fd, n.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
      if (f.fd < 0)
        return fail(errno == ENOENT ? ErrorCode::NotFound : ErrorCode::IoFailure,
                    "rooted open failed");
      struct stat st {};
      if (::fstat(f.fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
        return fail(ErrorCode::InvalidInput, "regular content required");
      if (static_cast<std::uint64_t>(st.st_size) > limit)
        return fail(ErrorCode::Exhausted, "content read limit");
      std::vector<std::byte> data(static_cast<std::size_t>(st.st_size));
      std::size_t done = 0;
      while (done != data.size()) {
        auto nread = ::read(f.fd, data.data() + done, data.size() - done);
        if (nread < 0 && errno == EINTR)
          continue;
        if (nread <= 0)
          return fail(ErrorCode::IoFailure, "content changed or read failed");
        done += static_cast<std::size_t>(nread);
      }
      std::byte extra;
      ssize_t nread;
      do {
        nread = ::read(f.fd, &extra, 1);
      } while (nread < 0 && errno == EINTR);
      if (nread != 0)
        return fail(ErrorCode::IoFailure, "content grew or read failed");
      return data;
    } catch (const std::bad_alloc &) {
      return fail(ErrorCode::Exhausted, "content read allocation");
    } catch (const std::length_error &) {
      return fail(ErrorCode::Exhausted, "content read capacity");
    }
  }
  Result<void> write(std::string_view name, std::span<const std::byte> data, bool immutable) {
    if (!valid_content_leaf(name))
      return fail(ErrorCode::InvalidInput, "invalid content leaf");
    try {
      std::string n(name), stage;
      Fd f{-1};
      // O_EXCL protects collision/replacement; bounded attempts, no random secret.
      for (int attempt = 0; attempt < 32 && f.fd < 0; ++attempt) {
        stage =
            ".race-stage-" + std::to_string(::getpid()) + "-" + std::to_string(next_.fetch_add(1));
        f.fd = ::openat(root_.fd, stage.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (f.fd < 0 && errno != EEXIST)
          return fail(ErrorCode::IoFailure, "content staging open failed");
      }
      if (f.fd < 0)
        return fail(ErrorCode::Busy, "content staging collisions");
      struct Cleanup {
        int root;
        const char *name;
        ~Cleanup() { ::unlinkat(root, name, 0); }
      } cleanup{root_.fd, stage.c_str()};
      std::size_t done = 0;
      while (done < data.size()) {
        auto nwrite = ::write(f.fd, data.data() + done, data.size() - done);
        if (nwrite < 0 && errno == EINTR)
          continue;
        if (nwrite <= 0)
          return fail(ErrorCode::IoFailure, "content staging write failed");
        done += static_cast<std::size_t>(nwrite);
      }
      int raw = f.fd;
      f.fd = -1;
      if (::close(raw))
        return fail(ErrorCode::IoFailure, "content staging close failed");
      if (immutable) {
        if (!::linkat(root_.fd, stage.c_str(), root_.fd, n.c_str(), 0))
          return {};
        if (errno != EEXIST)
          return fail(ErrorCode::IoFailure, "immutable publication failed");
        auto existing = read(name, data.size() ? data.size() : 1);
        if (!existing)
          return existing.error();
        if (existing.value().size() != data.size() ||
            !std::equal(data.begin(), data.end(), existing.value().begin()))
          return fail(ErrorCode::Duplicate, "immutable name has different bytes");
        return {};
      }
      struct stat st {};
      if (!::fstatat(root_.fd, n.c_str(), &st, AT_SYMLINK_NOFOLLOW) && !S_ISREG(st.st_mode))
        return fail(ErrorCode::InvalidInput, "catalog target is not regular");
      // renameat replaces a leaf itself; it cannot follow a swapped leaf symlink.
      if (::renameat(root_.fd, stage.c_str(), root_.fd, n.c_str()))
        return fail(ErrorCode::IoFailure, "catalog publication failed");
      return {};
    } catch (const std::bad_alloc &) {
      return fail(ErrorCode::Exhausted, "content publication allocation");
    }
  }
  Result<void> publish_immutable(std::string_view n, std::span<const std::byte> d) override {
    return write(n, d, true);
  }
  Result<void> replace(std::string_view n, std::span<const std::byte> d,
                       Durability policy) override {
    if (policy == Durability::Durable)
      return fail(ErrorCode::Unsupported, "durability unavailable");
    if (policy != Durability::AtomicVisibility)
      return fail(ErrorCode::InvalidInput, "invalid durability");
    return write(n, d, false);
  }
};
} // namespace
Result<std::shared_ptr<RootedFiles>> make_linux_rooted_files(std::string_view path) {
  if (path.empty() || path.front() != '/' || path.size() >= PATH_MAX ||
      path.find('\0') != path.npos)
    return fail(ErrorCode::InvalidInput, "absolute content root required");
  try {
    Fd dir{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (dir.fd < 0)
      return fail(ErrorCode::IoFailure, "root open failed");
    std::size_t at = 1;
    while (at < path.size()) {
      auto end = path.find('/', at);
      if (end == path.npos)
        end = path.size();
      auto part = path.substr(at, end - at);
      if (part.empty() || part == "." || part == "..")
        return fail(ErrorCode::InvalidInput, "noncanonical root");
      std::string name(part);
      int next = ::openat(dir.fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0)
        return fail(ErrorCode::IoFailure, "root component refused");
      ::close(dir.fd);
      dir.fd = next;
      at = end + 1;
    }
    auto owner = std::make_shared<Root>(dir.fd);
    dir.fd = -1;
    return std::shared_ptr<RootedFiles>(std::move(owner));
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "root owner allocation");
  }
}
} // namespace race::platform
