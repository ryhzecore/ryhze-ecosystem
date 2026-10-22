#include "test.hpp"
#include "race/platform/platform.hpp"
#include "race/platform/linux_test_hooks.hpp"
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
using namespace race;
void platform_tests() {
  auto created = platform::make_linux_services(); CHECK(created);
  auto services = std::move(created).value();
  auto before = services.clock->now();
  auto after = services.clock->now();
  CHECK(after.domain == before.domain && after.nanoseconds >= before.nanoseconds);
  std::atomic<int> value{0};
  auto thread = services.threads->start([&]{ value.store(42); }); CHECK(thread);
  CHECK(thread.value()->join()); CHECK(thread.value()->join()); CHECK(value == 42);
  CHECK_ERROR(services.threads->start({}), ErrorCode::InvalidInput);
  char folder[] = "/tmp/race-platform-XXXXXX"; CHECK(::mkdtemp(folder));
  std::string path = std::string(folder) + "/settings.txt";
  const std::string data = "validated configuration";
  auto bytes = std::as_bytes(std::span(data));
  CHECK(services.files->replace(path, bytes, platform::Durability::AtomicVisibility));
  auto read = services.files->read(path, 128); CHECK(read); CHECK(read.value().size() == data.size());
  CHECK(std::equal(read.value().begin(), read.value().end(), bytes.begin()));
  CHECK_ERROR(services.files->read(path, 2), ErrorCode::Exhausted);
  CHECK_ERROR(services.files->read("relative", 100), ErrorCode::InvalidInput);
  CHECK_ERROR(services.files->read(std::string("/tmp/x\0tail", 11), 100), ErrorCode::InvalidInput);
  CHECK_ERROR(services.files->replace(path, {}, platform::Durability::Durable), ErrorCode::Unsupported);
  CHECK_ERROR(services.files->replace(path, {}, static_cast<platform::Durability>(99)), ErrorCode::InvalidInput);
  CHECK(services.files->read(path, 128).value().size() == data.size());
  auto preserved = [&] {
    auto original = services.files->read(path, 128); CHECK(original);
    CHECK(original.value().size() == bytes.size());
    CHECK(std::equal(original.value().begin(), original.value().end(), bytes.begin()));
    for (const auto& item : std::filesystem::directory_iterator(folder))
      CHECK(!item.path().filename().string().starts_with(".race-stage-"));
  };
  const std::string replacement = "different replacement contents after validation";
  auto replacement_bytes = std::as_bytes(std::span(replacement));
  auto injected = platform::linux_files_with_write_fault(3); CHECK(injected);
  CHECK_ERROR(injected.value()->replace(path, replacement_bytes, platform::Durability::AtomicVisibility), ErrorCode::IoFailure);
  preserved();
  for (auto fault : {platform::LinuxFileFault::Close, platform::LinuxFileFault::Rename}) {
    auto failed = platform::linux_files_with_fault(fault); CHECK(failed);
    CHECK_ERROR(failed.value()->replace(path, replacement_bytes, platform::Durability::AtomicVisibility), ErrorCode::IoFailure);
    preserved();
  }
  auto interrupted = platform::linux_files_with_fault(platform::LinuxFileFault::WriteInterruptedOnce); CHECK(interrupted);
  CHECK(interrupted.value()->replace(path, replacement_bytes, platform::Durability::AtomicVisibility));
  auto after_interruption = services.files->read(path, 128); CHECK(after_interruption);
  CHECK(after_interruption.value().size() == replacement_bytes.size());
  CHECK(std::equal(after_interruption.value().begin(), after_interruption.value().end(), replacement_bytes.begin()));
  CHECK(services.files->replace(path, bytes, platform::Durability::AtomicVisibility));
  preserved();
  CHECK_ERROR(platform::linux_files_with_fault(static_cast<platform::LinuxFileFault>(99)), ErrorCode::InvalidInput);
  CHECK(::chmod(folder,0500) == 0);
  CHECK_ERROR(services.files->replace(path, bytes, platform::Durability::AtomicVisibility), ErrorCode::IoFailure);
  CHECK(::chmod(folder,0700) == 0);
  preserved();
  auto symlink = std::string(folder) + "/symlink";
  std::filesystem::create_symlink(path, symlink);
  CHECK_ERROR(services.files->read(symlink, 128), ErrorCode::IoFailure);
  CHECK_ERROR(services.files->replace(symlink, {}, platform::Durability::AtomicVisibility), ErrorCode::InvalidInput);
  CHECK_ERROR(services.files->replace(std::string(folder)+"/missing/a", {}, platform::Durability::AtomicVisibility), ErrorCode::IoFailure);
  CHECK(services.files->read(path, 128).value().size() == data.size());
  CHECK_ERROR(services.files->read(folder, 128), ErrorCode::InvalidInput);
  CHECK(services.files->replace(path, {}, platform::Durability::AtomicVisibility));
  CHECK(services.files->read(path, 128).value().empty());
  std::filesystem::remove_all(folder);
  auto memory = platform::linux_memory_info(); CHECK(memory); CHECK(memory.value().physical_bytes > 0); CHECK(memory.value().resident_bytes > 0);
}
