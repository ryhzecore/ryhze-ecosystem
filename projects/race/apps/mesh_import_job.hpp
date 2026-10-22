#pragma once
#include "mesh_pipeline.hpp"
#include "race/platform/process.hpp"

namespace race::mesh_import {
// Trusted first-party authoring composition, not an Assets runtime service or
// filesystem/network sandbox. Caller supplies a fresh, retained staging root.
// input.obj and result.rasset are reserved. Staging is preserved after failure;
// cleanup belongs to the composition after Job destruction has quiesced process.
// stage_directory must name exactly staging's root, different from output's
// actual root. Pointer inequality alone does not establish this caller contract.
class Job {
  std::shared_ptr<platform::RootedFiles> staging_,output_;
  std::vector<std::byte> source_;
  assets::AssetId id_{};
  assets::mesh::Stamp expected_;
  mesh_pipeline::Profile profile_{mesh_pipeline::Profile::Geometry};
  platform::process::Request request_;
  bool cancelled_{},publication_attempted_{};
  // Destruction order matters: process quiesces before source and roots release.
  std::unique_ptr<platform::process::Service> process_;
  Job()=default;
public:
  static Result<std::unique_ptr<Job>> start(
      std::unique_ptr<platform::process::Service>,
      std::shared_ptr<platform::RootedFiles> staging,
      std::shared_ptr<platform::RootedFiles> output,
      std::string_view executable,std::string_view stage_directory,
      assets::AssetId,std::span<const std::byte> source,
      mesh_pipeline::Profile=mesh_pipeline::Profile::Geometry);
  ~Job()=default;
  Job(const Job&)=delete;
  platform::process::Request request() const noexcept {return request_;}
  std::size_t retained_source_bytes() const noexcept {return source_.size();}
  Result<platform::process::Snapshot> poll() noexcept;
  Result<platform::process::Snapshot> wait(std::uint32_t milliseconds) noexcept;
  Result<std::span<const std::byte>> stderr_prefix() const noexcept;
  // Sticky even after child completion but before publication. No publication
  // is attempted by poll/wait; they retain the source and completed request.
  Result<void> cancel() noexcept;
  // Shares the single terminal attempt with publish(). Returns validated owned
  // imported bytes for a composition that will cook and publish them itself.
  Result<std::vector<std::byte>> take_product();
  // Synchronous bounded read, full domain admission, immutable write/readback.
  // At most one terminal attempt. Readback failure may leave a complete product.
  Result<std::string> publish();
};
} // namespace race::mesh_import
