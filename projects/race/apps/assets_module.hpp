#pragma once
#include "race/assets/assets.hpp"
#include "race/runtime/lifecycle.hpp"
// Executable composition adapter; not an Assets-to-Runtime library dependency.
namespace race::fixtures {
class AssetsModule final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_{
      "assets-headless", 1, {}, 0, runtime::Memory | runtime::Jobs};
  std::shared_ptr<platform::RootedFiles> files_;
  platform::Threads &threads_;
  assets::Catalog catalog_;
  runtime::ModuleContext context_{};

public:
  std::unique_ptr<assets::Service> service;
  AssetsModule(std::shared_ptr<platform::RootedFiles> f, platform::Threads &t, assets::Catalog c)
      : files_(std::move(f)), threads_(t), catalog_(std::move(c)) {}
  const runtime::ModuleDescriptor &descriptor() const noexcept override { return descriptor_; }
  Result<void> validate() noexcept override { return assets::validate_catalog(catalog_, {}); }
  Result<void> initialize(runtime::ModuleContext c) noexcept override {
    context_ = c;
    if (!c.memory || !c.jobs)
      return fail(ErrorCode::InvalidInput, "asset services missing");
    auto s = assets::Service::create(std::move(catalog_), {}, *c.memory, files_, threads_, *c.jobs);
    if (!s)
      return s.error();
    service = std::move(s.value());
    return {};
  }
  Result<void> start() noexcept override { return {}; }
  void request_stop() noexcept override {
    if (service)
      service->request_stop();
  }
  Result<void> quiesce(std::chrono::milliseconds timeout) noexcept override {
    return service ? service->stop(timeout) : Result<void>{};
  }
  void shutdown() noexcept override { service.reset(); }
  void rollback_initialization() noexcept override { service.reset(); }
};
} // namespace race::fixtures
