#pragma once
#include "mesh_import_job.hpp"
#include <thread>

namespace race::mesh_import {
struct Desired {
  std::uint64_t owner{}, generation{};
  assets::Digest source{};
  friend bool operator==(const Desired&,const Desired&)=default;
};
struct ReimportLimits {
  std::uint64_t generations{~std::uint64_t{0}};
  assets::mesh::Limits residency{};
};
// Executable composition for one stable mesh, one worker, no pending queue.
// Caller supplies a never-reused owner identity. All operations except acquire
// and residency require the creating thread; destruction also belongs there.
// Every desire() invalidates the previous token, even for identical bytes.
// No filesystem watching or Scene reference rewrite is performed here.
class Reimport {
  std::thread::id thread_{std::this_thread::get_id()};
  std::uint64_t identity_{}, generation_{}, attempted_{};
  assets::AssetId id_{};
  mesh_pipeline::Profile profile_{mesh_pipeline::Profile::Geometry};
  ReimportLimits limits_;
  std::shared_ptr<platform::RootedFiles> output_;
  std::unique_ptr<assets::mesh::Store> store_;
  std::optional<Desired> desired_, active_;
  std::optional<assets::mesh::Update> update_;
  // Last member: quiesces before roots and the Store release on destruction.
  std::unique_ptr<Job> job_;
  Reimport()=default;
  bool owner_thread() const noexcept;
  void clear() noexcept;
public:
  static Result<std::unique_ptr<Reimport>> create(std::uint64_t,assets::AssetId,
      std::shared_ptr<platform::RootedFiles>,TrackedAllocator,const ReimportLimits& = {},
      mesh_pipeline::Profile=mesh_pipeline::Profile::Geometry);
  ~Reimport()=default;
  Reimport(const Reimport&)=delete;
  mesh_pipeline::Profile profile() const noexcept{return profile_;}
  // Initial explicit checkpoint restoration only: creating thread, no previous
  // desire/attempt/job or current revision. Validates and copies cooked bytes;
  // failed admission leaves an empty owner retryable, without disk publication.
  Result<assets::mesh::ResourceHandle> restore_initial(std::span<const std::byte>);
  // Exhaustion is terminal: invalidate/cancel pending admission but keep the
  // current revision and live job. Stop failure also leaves no desired token.
  Result<Desired> desire(assets::Digest);
  // A source-change hint can invalidate admission before its replacement bytes
  // are readable. Does not consume a generation or relinquish the live job.
  Result<void> invalidate();
  // One attempt per generation; Busy does not consume it. A completed job still
  // occupies the slot until finish/reap. Job's fresh staging contract applies.
  Result<void> start(Desired,std::unique_ptr<platform::process::Service>,
      std::shared_ptr<platform::RootedFiles> staging,std::string_view executable,
      std::string_view stage_directory,std::span<const std::byte> source);
  Result<platform::process::Snapshot> poll() noexcept;
  Result<platform::process::Snapshot> wait(std::uint32_t) noexcept;
  // Synchronous cook/disk readback/Store publication. Busy and stale tokens do
  // not consume a live job. Every matching terminal attempt clears the slot.
  // Disk products can survive failure without becoming the current revision.
  Result<assets::mesh::ResourceHandle> finish(Desired);
  // Discard only after matching process+stream quiescence; never detaches.
  Result<void> reap() noexcept;
  Result<assets::mesh::ResourceHandle> current() const;
  Result<assets::mesh::Lease> acquire(assets::mesh::ResourceHandle) const;
  assets::mesh::Stats residency() const;
  Result<std::size_t> pending_source_bytes() const noexcept;
};
} // namespace race::mesh_import
