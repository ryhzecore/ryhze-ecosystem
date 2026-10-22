#pragma once
#include "race/core/jobs.hpp"
#include "race/core/memory.hpp"
#include "race/foundation/identity.hpp"
#include "race/platform/rooted_files.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace race::assets {
using AssetId = PersistentId;
using Digest = std::array<std::byte, 32>;
inline constexpr std::uint32_t fixture_type = 1, linux_target = 1, wire_version = 1;
inline constexpr std::size_t hard_max_entries = 128, hard_max_dependencies = 16;
struct Limits {
  std::size_t entries{64}, dependencies{8}, depth{16}, values{65536};
  std::size_t product_bytes{1024 * 1024}, catalog_bytes{256 * 1024};
  std::size_t requests{64}, resources{64}, io_queue{16}, decode_jobs{4};
  std::size_t temporary_bytes{8 * 1024 * 1024}, retained_bytes{16 * 1024 * 1024},
      retained_revisions{128};
};
Result<void> validate_limits(const Limits &) noexcept;
Digest digest(std::span<const std::byte>) noexcept;
std::string hex(Digest);
std::string id_text(AssetId);
Result<AssetId> parse_id(std::string_view) noexcept;
struct Dependency {
  AssetId id;
  Digest revision{};
  friend bool operator==(const Dependency &, const Dependency &) = default;
};
struct Entry {
  AssetId id;
  std::uint32_t type{fixture_type}, target{linux_target}, schema{1};
  Digest revision{};
  std::uint64_t bytes{};
  std::string location;
  std::vector<Dependency> hard;
  std::vector<AssetId> soft;
};
struct Catalog {
  std::vector<Entry> entries;
};
enum class Stage { Catalog, Admission, Reading, Decode, Dependency, Publish, Stop };
struct Diagnostic {
  AssetId asset{}, cause{};
  Stage stage{Stage::Catalog};
  ErrorCode code{ErrorCode::InvalidInput};
  std::array<AssetId, hard_max_entries> path{};
  std::size_t path_size{};
};
Result<void> validate_catalog(const Catalog &, const Limits &, Diagnostic * = nullptr);
Result<std::vector<std::byte>> encode_catalog(const Catalog &, const Limits & = {});
Result<Catalog> decode_catalog(std::span<const std::byte>, const Limits & = {},
                               Diagnostic * = nullptr);
struct Input {
  AssetId id;
  std::uint64_t seed{1}, count{16}, settings{0}, environment{1};
  std::vector<AssetId> hard, soft;
};
// Strict readable fixture input; no parser in runtime loading.
Result<Input> parse_input(std::string_view, const Limits & = {});
struct ProductView {
  AssetId id;
  std::uint64_t seed{}, count{}, settings{}, environment{};
  std::span<const std::byte> payload;
};
Result<ProductView> validate_product(std::span<const std::byte>, const Entry &,
                                     const Limits & = {});
Result<Catalog> cook(std::span<const Input>, platform::RootedFiles &, const Limits & = {},
                     Diagnostic * = nullptr);
std::uint64_t fixture_value(std::uint64_t seed, std::uint64_t index) noexcept;

struct Accounting;
class FixtureResource {
  AssetId id_;
  Digest revision_;
  OwnedBlock block_;
  std::size_t count_;
  std::shared_ptr<Accounting> accounting_;
  std::vector<std::shared_ptr<const FixtureResource>> dependencies_;

public:
  FixtureResource(AssetId, Digest, OwnedBlock, std::size_t, std::shared_ptr<Accounting>,
                  std::vector<std::shared_ptr<const FixtureResource>>);
  ~FixtureResource();
  AssetId id() const noexcept { return id_; }
  Digest revision() const noexcept { return revision_; }
  std::span<const std::uint64_t> values() const noexcept;
};
using ResourceHandle = Handle<const FixtureResource>;
using Lease = AccessScope<const FixtureResource>;
struct Request;
using RequestHandle = Handle<Request>;
enum class RequestState { Pending, Ready, Failed, Cancelled };
enum class ResourceState {
  Dormant,
  Queued,
  Reading,
  Decoding,
  DependencyWaiting,
  Ready,
  Failed,
  Cancelled,
  Retiring
};
struct Receipt {
  RequestState state{RequestState::Pending};
  ResourceHandle resource{};
  Diagnostic diagnostic{};
};
struct Stats {
  std::size_t payload_bytes{}, peak_payload_bytes{}, temporary_bytes{}, peak_temporary_bytes{};
  std::size_t retained_revisions{}, peak_retained_revisions{}, bounded_metadata_capacity_bytes{};
  std::size_t queue_depth{}, in_flight{}, requests{}, decode_jobs{};
  std::uint64_t reads{}, decodes{}, publications{}, failures{}, read_nanoseconds{},
      decode_nanoseconds{}, drain_nanoseconds{};
};
struct ServiceState;
class Service {
  std::shared_ptr<ServiceState> state_;
  std::unique_ptr<platform::Thread> io_;
  Service(std::shared_ptr<ServiceState>);

public:
  // Scheduler and Threads outlive Service; Files and allocator state are retained.
  static Result<std::unique_ptr<Service>> create(const Catalog &, Limits, TrackedAllocator,
                                                 std::shared_ptr<platform::RootedFiles>,
                                                 platform::Threads &, core::Scheduler &);
  ~Service(); // Owner thread; blocks until quiescent, never detaches.
  Service(const Service &) = delete;
  Result<RequestHandle> request(AssetId, std::uint32_t type = fixture_type);
  Result<Receipt> poll(RequestHandle) const;
  Result<void> cancel(RequestHandle);
  Result<void> release(RequestHandle); // Also cancels a pending subscriber.
  Result<Lease> acquire(ResourceHandle, std::uint32_t type = fixture_type) const;
  Result<void> retire(ResourceHandle); // Owner thread; existing leases remain valid.
  Result<void> drain();                // Owner thread; inspects/releases jobs before publication.
  void request_stop() noexcept;
  Result<void> stop(std::chrono::milliseconds);
  Result<ResourceState> state(AssetId) const;
  Stats stats() const;
};
} // namespace race::assets
