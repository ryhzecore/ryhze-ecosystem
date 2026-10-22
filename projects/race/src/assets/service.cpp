#include "race/assets/assets.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <thread>

namespace race::assets {
using Clock = std::chrono::steady_clock;
namespace {
std::uint64_t elapsed(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count();
}
} // namespace
struct Accounting {
  mutable std::mutex mutex;
  std::size_t bytes{}, peak{}, temporary{}, peak_temporary{}, revisions{}, peak_revisions{};
  Limits limits;
  explicit Accounting(Limits l) : limits(l) {}
  bool reserve(std::size_t n) {
    std::lock_guard lock(mutex);
    if (n > limits.retained_bytes - bytes || revisions == limits.retained_revisions)
      return false;
    bytes += n;
    ++revisions;
    peak = std::max(peak, bytes);
    peak_revisions = std::max(peak_revisions, revisions);
    return true;
  }
  void release(std::size_t n) {
    std::lock_guard lock(mutex);
    bytes -= n;
    --revisions;
  }
  bool reserve_temp(std::size_t n) {
    std::lock_guard lock(mutex);
    if (n > limits.temporary_bytes - temporary)
      return false;
    temporary += n;
    peak_temporary = std::max(peak_temporary, temporary);
    return true;
  }
  void release_temp(std::size_t n) {
    std::lock_guard lock(mutex);
    temporary -= n;
  }
};
FixtureResource::FixtureResource(AssetId id, Digest revision, OwnedBlock block, std::size_t count,
                                 std::shared_ptr<Accounting> accounting,
                                 std::vector<std::shared_ptr<const FixtureResource>> deps)
    : id_(id), revision_(revision), block_(std::move(block)), count_(count),
      accounting_(std::move(accounting)), dependencies_(std::move(deps)) {}
FixtureResource::~FixtureResource() {
  auto bytes = block_.size();
  block_.reset();
  accounting_->release(bytes);
}
std::span<const std::uint64_t> FixtureResource::values() const noexcept {
  return {reinterpret_cast<const std::uint64_t *>(block_.data()), count_};
}
struct Request {
  std::size_t index;
  Receipt receipt;
  std::shared_ptr<const FixtureResource> retained;
  explicit Request(std::size_t i) : index(i){};
};
namespace {
struct Load {
  const Entry *entry;
  Limits limits;
  TrackedAllocator allocator;
  std::shared_ptr<Accounting> accounting;
  std::vector<std::byte> bytes;
  OwnedBlock payload;
  std::size_t count{}, temporary{};
  bool reserved{}, read_done{}, abandoned{};
  std::optional<Error> error;
  std::optional<core::GroupHandle> group;
  std::optional<core::JobHandle> job;
  std::uint64_t read_ns{}, decode_ns{};
  Load(const Entry *e, Limits l, TrackedAllocator a, std::shared_ptr<Accounting> ac)
      : entry(e), limits(l), allocator(std::move(a)), accounting(std::move(ac)) {}
  ~Load() {
    auto n = payload.size();
    payload.reset();
    std::vector<std::byte>().swap(bytes);
    if (reserved)
      accounting->release(n);
    if (temporary)
      accounting->release_temp(temporary);
  }
};
struct Record {
  ResourceState state{ResourceState::Dormant};
  std::shared_ptr<Load> load;
  std::shared_ptr<const FixtureResource> value;
  std::optional<ResourceHandle> handle;
  Diagnostic diagnostic;
  bool retired{};
};
Result<void> decode(const core::JobContext &ctx) {
  auto &work = *static_cast<Load *>(ctx.context());
  auto start = Clock::now();
  if (ctx.cancelled())
    return fail(ErrorCode::Cancelled, "asset decode cancelled");
  auto parsed = validate_product(work.bytes, *work.entry, work.limits);
  if (!parsed) {
    work.decode_ns = elapsed(start);
    return parsed.error();
  }
  auto n = static_cast<std::size_t>(parsed.value().count) * 8;
  if (!work.accounting->reserve(n))
    return fail(ErrorCode::Exhausted, "retained asset budget");
  auto block = work.allocator.allocate({n, alignof(std::uint64_t), 6, 6});
  if (!block) {
    work.accounting->release(n);
    return block.error();
  }
  work.payload = std::move(block.value());
  work.reserved = true;
  work.count = parsed.value().count;
  auto *values = reinterpret_cast<std::uint64_t *>(work.payload.data());
  auto bytes = parsed.value().payload;
  for (std::size_t i = 0; i < work.count; ++i) {
    if ((i & 1023) == 0 && ctx.cancelled())
      return fail(ErrorCode::Cancelled, "asset decode cancelled");
    std::uint64_t v = 0;
    for (std::size_t j = 0; j < 8; ++j)
      v |= std::uint64_t(std::to_integer<unsigned char>(bytes[i * 8 + j])) << (j * 8);
    values[i] = v;
  }
  work.decode_ns = elapsed(start);
  return {};
}
} // namespace
struct ServiceState {
  Catalog catalog;
  Limits limits;
  TrackedAllocator allocator;
  std::shared_ptr<platform::RootedFiles> files;
  core::Scheduler &jobs;
  std::shared_ptr<Accounting> accounting;
  HandleTable<Request> requests;
  HandleTable<const FixtureResource> resources;
  std::vector<std::pair<RequestHandle, std::shared_ptr<Request>>> subscribers;
  std::vector<Record> records;
  mutable std::mutex mutex;
  std::condition_variable changed;
  bool stopping{}, io_done{};
  const std::thread::id owner = std::this_thread::get_id();
  Stats counters;
  ServiceState(Catalog c, Limits l, TrackedAllocator a, std::shared_ptr<platform::RootedFiles> f,
               core::Scheduler &j)
      : catalog(std::move(c)), limits(l), allocator(std::move(a)), files(std::move(f)), jobs(j),
        accounting(std::make_shared<Accounting>(l)), requests(l.requests), resources(l.resources),
        records(catalog.entries.size()) {
    subscribers.reserve(l.requests);
    counters.bounded_metadata_capacity_bytes =
        sizeof(ServiceState) +
        l.requests *
            (sizeof(Request) + sizeof(std::pair<RequestHandle, std::shared_ptr<Request>>) + 128) +
        l.entries *
            (sizeof(Entry) + sizeof(Record) + 2 * l.dependencies * sizeof(Dependency) + 96) +
        l.resources * (sizeof(Load) + 128) +
        l.retained_revisions *
            (sizeof(FixtureResource) +
             l.dependencies * sizeof(std::shared_ptr<const FixtureResource>) + 128);
  }
  std::size_t index(AssetId id) const {
    for (std::size_t i = 0; i < catalog.entries.size(); ++i)
      if (catalog.entries[i].id == id)
        return i;
    return catalog.entries.size();
  }
  void closure(std::size_t i, std::array<bool, hard_max_entries> &mask) const {
    if (mask[i])
      return;
    mask[i] = true;
    for (auto d : catalog.entries[i].hard)
      closure(index(d.id), mask);
  }
  std::array<bool, hard_max_entries> needed() const {
    std::array<bool, hard_max_entries> mask{};
    for (const auto &[h, r] : subscribers) {
      (void)h;
      if (r->receipt.state == RequestState::Pending ||
          (r->receipt.state == RequestState::Ready && records[r->index].handle &&
           r->receipt.resource == *records[r->index].handle))
        closure(r->index, mask);
    }
    return mask;
  }
  void failure(std::size_t i, ErrorCode error, Stage stage, AssetId cause = {}) {
    auto &r = records[i];
    r.state = error == ErrorCode::Cancelled ? ResourceState::Cancelled : ResourceState::Failed;
    r.diagnostic = {catalog.entries[i].id, cause, stage, error, {}, 0};
    ++counters.failures;
  }
};
namespace {
void io_loop(const std::shared_ptr<ServiceState> &s) {
  std::unique_lock lock(s->mutex);
  for (;;) {
    s->changed.wait(lock, [&] {
      return s->stopping || std::any_of(s->records.begin(), s->records.end(), [](const Record &r) {
               return r.state == ResourceState::Queued;
             });
    });
    if (s->stopping)
      break;
    auto needed = s->needed();
    std::size_t index = 0;
    for (; index < s->records.size(); ++index)
      if (s->records[index].state == ResourceState::Queued)
        break;
    auto &r = s->records[index];
    if (!needed[index]) {
      r.state = ResourceState::Cancelled;
      r.load.reset();
      continue;
    }
    auto work = r.load;
    if (!s->accounting->reserve_temp(work->entry->bytes)) {
      s->failure(index, ErrorCode::Exhausted, Stage::Reading);
      r.load.reset();
      continue;
    }
    work->temporary = work->entry->bytes;
    r.state = ResourceState::Reading;
    ++s->counters.reads;
    lock.unlock();
    auto start = Clock::now();
    Result<std::vector<std::byte>> bytes =
        fail(ErrorCode::IoFailure, "asset I/O adapter exception");
    try {
      bytes = s->files->read(work->entry->location, work->entry->bytes);
    } catch (const std::bad_alloc &) {
      bytes = fail(ErrorCode::Exhausted, "asset I/O allocation");
    } catch (...) {
      bytes = fail(ErrorCode::IoFailure, "asset I/O adapter exception");
    }
    work->read_ns = elapsed(start);
    if (bytes)
      work->bytes = std::move(bytes.value());
    else
      work->error = bytes.error();
    lock.lock();
    work->read_done = true;
    s->changed.notify_all();
  }
  s->io_done = true;
  s->changed.notify_all();
}
} // namespace
Service::Service(std::shared_ptr<ServiceState> s) : state_(std::move(s)) {}
Result<std::unique_ptr<Service>> Service::create(const Catalog &catalog, Limits limits,
                                                 TrackedAllocator allocator,
                                                 std::shared_ptr<platform::RootedFiles> files,
                                                 platform::Threads &threads,
                                                 core::Scheduler &jobs) {
  try {
    auto check = validate_catalog(catalog, limits);
    if (!check)
      return check.error();
    if (!files)
      return fail(ErrorCode::InvalidInput, "rooted files required");
    Catalog bounded{catalog.entries};
    auto state = std::make_shared<ServiceState>(std::move(bounded), limits, std::move(allocator),
                                                std::move(files), jobs);
    auto service = std::unique_ptr<Service>(new Service(state));
    auto thread = [&]() -> Result<std::unique_ptr<platform::Thread>> {
      try {
        return threads.start([state] { io_loop(state); });
      } catch (const std::bad_alloc &) {
        return fail(ErrorCode::Exhausted, "asset I/O start allocation");
      } catch (...) {
        return fail(ErrorCode::PlatformFailure, "asset I/O start failure");
      }
    }();
    if (!thread) {
      state->io_done = true;
      return thread.error();
    }
    service->io_ = std::move(thread.value());
    return service;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "asset service allocation");
  }
}
Service::~Service() {
  if (state_->owner != std::this_thread::get_id())
    std::abort();
  for (;;) {
    auto r = stop(std::chrono::milliseconds::max());
    if (r)
      break;
    if (r.error().code != ErrorCode::Exhausted && r.error().code != ErrorCode::Busy)
      std::abort();
    std::this_thread::yield();
  }
}
Result<RequestHandle> Service::request(AssetId id, std::uint32_t type) {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  if (s.stopping)
    return fail(ErrorCode::Stopping, "asset admission stopped");
  if (!id.valid())
    return fail(ErrorCode::InvalidInput, "zero required asset");
  auto index = s.index(id);
  if (index == s.records.size())
    return fail(ErrorCode::NotFound, "asset absent from catalog");
  if (type != s.catalog.entries[index].type)
    return fail(ErrorCode::InvalidInput, "asset type mismatch");
  if (s.subscribers.size() == s.limits.requests)
    return fail(ErrorCode::QueueFull, "asset request capacity");
  try {
    std::array<bool, hard_max_entries> mask{};
    s.closure(index, mask);
    std::size_t active = 0, queued = 0, additional = 0;
    for (std::size_t i = 0; i < s.records.size(); ++i) {
      const auto &r = s.records[i];
      if (mask[i] && r.load && r.load->abandoned)
        return fail(ErrorCode::Busy, "previous cancelled load still draining");
      if (r.load || r.value)
        ++active;
      if (r.state == ResourceState::Queued)
        ++queued;
      if (mask[i] && !r.load && !r.value)
        ++additional;
    }
    if (additional > s.limits.resources - std::min(s.limits.resources, active))
      return fail(ErrorCode::Exhausted, "asset resource capacity");
    if (additional > s.limits.io_queue - std::min(s.limits.io_queue, queued))
      return fail(ErrorCode::QueueFull, "asset I/O queue capacity");
    std::array<std::shared_ptr<Load>, hard_max_entries> prepared{};
    for (std::size_t i = 0; i < s.records.size(); ++i)
      if (mask[i] && !s.records[i].load && !s.records[i].value)
        prepared[i] =
            std::make_shared<Load>(&s.catalog.entries[i], s.limits, s.allocator, s.accounting);
    auto request = std::make_shared<Request>(index);
    auto handle = s.requests.insert(request);
    if (!handle)
      return handle.error();
    // All fallible admission has completed. Reserved subscriber vector cannot grow.
    s.subscribers.emplace_back(handle.value(), request);
    for (std::size_t i = 0; i < s.records.size(); ++i)
      if (prepared[i]) {
        auto &r = s.records[i];
        r.load = std::move(prepared[i]);
        r.state = ResourceState::Queued;
        r.retired = false;
        r.diagnostic = {};
      }
    s.changed.notify_one();
    return handle;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "asset request allocation");
  }
}
Result<Receipt> Service::poll(RequestHandle h) const {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  auto r = s.requests.resolve(h);
  if (!r)
    return r.error();
  return r.value()->receipt;
}
Result<void> Service::cancel(RequestHandle h) {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  auto r = s.requests.resolve(h);
  if (!r)
    return r.error();
  if (r.value()->receipt.state == RequestState::Pending) {
    r.value()->receipt.state = RequestState::Cancelled;
    r.value()->receipt.diagnostic = {
        s.catalog.entries[r.value()->index].id, {}, Stage::Admission, ErrorCode::Cancelled, {}, 0};
  }
  s.changed.notify_one();
  return {};
}
Result<void> Service::release(RequestHandle h) {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  auto r = s.requests.resolve(h);
  if (!r)
    return r.error();
  auto erased = s.requests.erase(h);
  if (!erased)
    return erased;
  auto it = std::find_if(s.subscribers.begin(), s.subscribers.end(),
                         [&](const auto &p) { return p.first == h; });
  s.subscribers.erase(it);
  s.changed.notify_one();
  return {};
}
Result<Lease> Service::acquire(ResourceHandle h, std::uint32_t type) const {
  if (type != fixture_type)
    return fail(ErrorCode::InvalidInput, "resource handle type mismatch");
  return state_->resources.resolve(h);
}
Result<void> Service::retire(ResourceHandle h) {
  auto &s = *state_;
  if (s.owner != std::this_thread::get_id())
    return fail(ErrorCode::WrongThread, "asset owner required");
  std::lock_guard lock(s.mutex);
  auto lease = s.resources.resolve(h);
  if (!lease)
    return lease.error();
  auto i = s.index(lease.value()->id());
  auto &r = s.records[i];
  for (const auto &[rh, request] : s.subscribers) {
    (void)rh;
    if (request->receipt.state == RequestState::Pending) {
      std::array<bool, hard_max_entries> needed{};
      s.closure(request->index, needed);
      if (needed[i])
        return fail(ErrorCode::Busy, "resource required by pending parent");
    }
  }
  if (!r.handle || *r.handle != h)
    return fail(ErrorCode::StaleHandle, "resource no longer indexed");
  auto erased = s.resources.erase(h);
  if (!erased)
    return erased;
  r.handle.reset();
  r.value.reset();
  r.retired = true;
  r.state = ResourceState::Retiring;
  return {};
}
Result<ResourceState> Service::state(AssetId id) const {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  auto i = s.index(id);
  if (i == s.records.size())
    return fail(ErrorCode::NotFound, "asset absent");
  return s.records[i].state;
}
Result<void> Service::drain() {
  auto &s = *state_;
  if (s.owner != std::this_thread::get_id())
    return fail(ErrorCode::WrongThread, "asset drain owner required");
  auto start = Clock::now();
  std::lock_guard lock(s.mutex);
  auto wanted = s.needed();
  try {
    std::size_t running = 0;
    for (auto &r : s.records)
      if (r.load && r.load->group)
        ++running;
    for (std::size_t i = 0; i < s.records.size(); ++i) {
      auto &r = s.records[i];
      auto work = r.load;
      if (!work)
        continue;
      if (!wanted[i] || s.stopping) {
        work->abandoned = true;
        if (work->group) {
          auto cancel = s.jobs.cancel(*work->group);
          if (!cancel)
            return cancel;
        } else if (r.state != ResourceState::Reading || work->read_done) {
          r.load.reset();
          r.state = ResourceState::Cancelled;
          continue;
        }
      }
      if (r.state == ResourceState::Reading && work->read_done) {
        s.counters.read_nanoseconds += work->read_ns;
        work->read_ns = 0;
        if (work->error) {
          s.failure(i, work->error->code, Stage::Reading);
          r.load.reset();
          continue;
        }
        if (!wanted[i] || s.stopping) {
          r.load.reset();
          r.state = ResourceState::Cancelled;
          continue;
        }
        if (running >= s.limits.decode_jobs)
          continue;
        auto group = s.jobs.create_group();
        if (!group) {
          s.failure(i, group.error().code, Stage::Decode);
          r.load.reset();
          continue;
        }
        work->group = group.value();
        core::JobDesc desc;
        desc.function = decode;
        desc.owned_context = work;
        desc.unknown_access = false;
        auto job = s.jobs.submit(*work->group, desc);
        auto close = s.jobs.close(*work->group);
        if (!close)
          return close;
        if (!job) {
          work->error = job.error();
        } else
          work->job = job.value();
        r.state = ResourceState::Decoding;
        ++running;
        if (job)
          ++s.counters.decodes;
      }
      if (r.state == ResourceState::Decoding) {
        auto done = s.jobs.wait(*work->group, std::chrono::milliseconds(0));
        if (!done) {
          if (done.error().code == ErrorCode::Timeout)
            continue;
          return done.error();
        }
        if (work->job) {
          auto result = s.jobs.completion(*work->job);
          if (!result)
            return result.error();
          if (!result.value().terminal())
            return fail(ErrorCode::Invariant, "asset job not terminal after wait");
          if (result.value().state != core::JobState::Succeeded)
            work->error = result.value().error;
        }
        auto released = s.jobs.release(*work->group);
        if (!released)
          return released;
        work->group.reset();
        work->job.reset();
        --running;
        s.counters.decode_nanoseconds += work->decode_ns;
        std::vector<std::byte>().swap(work->bytes);
        s.accounting->release_temp(work->temporary);
        work->temporary = 0;
        if (!wanted[i] || s.stopping) {
          r.load.reset();
          r.state = ResourceState::Cancelled;
          continue;
        }
        if (work->error) {
          s.failure(i, work->error->code, Stage::Decode);
          r.load.reset();
          continue;
        }
        r.state = ResourceState::DependencyWaiting;
      }
    }
    // Topological progress is bounded by catalog size; ordering is not a readiness assumption.
    for (std::size_t pass = 0; pass < s.records.size(); ++pass) {
      bool progress = false;
      for (std::size_t i = 0; i < s.records.size(); ++i) {
        auto &r = s.records[i];
        if (r.state != ResourceState::DependencyWaiting)
          continue;
        bool ready = true;
        std::optional<AssetId> failed;
        for (auto dep : s.catalog.entries[i].hard) {
          const auto &dr = s.records[s.index(dep.id)];
          if (dr.state == ResourceState::Failed || dr.state == ResourceState::Cancelled) {
            failed = dr.diagnostic.cause.valid() ? dr.diagnostic.cause : dep.id;
            break;
          }
          if (dr.state != ResourceState::Ready)
            ready = false;
        }
        if (failed) {
          s.failure(i, ErrorCode::FailedPrerequisite, Stage::Dependency, *failed);
          r.load.reset();
          progress = true;
          continue;
        }
        if (!ready)
          continue;
        auto work = r.load;
        std::vector<std::shared_ptr<const FixtureResource>> deps;
        deps.reserve(s.catalog.entries[i].hard.size());
        for (auto dep : s.catalog.entries[i].hard)
          deps.push_back(s.records[s.index(dep.id)].value);
        auto value = std::make_shared<FixtureResource>(
            s.catalog.entries[i].id, s.catalog.entries[i].revision, std::move(work->payload),
            work->count, s.accounting, std::move(deps));
        work->reserved = false;
        auto handle = s.resources.insert(value);
        if (!handle) {
          s.failure(i, handle.error().code, Stage::Publish);
          r.load.reset();
          progress = true;
          continue;
        }
        r.value = std::move(value);
        r.handle = handle.value();
        r.load.reset();
        r.state = ResourceState::Ready;
        r.retired = false;
        ++s.counters.publications;
        progress = true;
      }
      if (!progress)
        break;
    }
    for (auto &[h, request] : s.subscribers) {
      (void)h;
      if (request->receipt.state != RequestState::Pending)
        continue;
      auto &r = s.records[request->index];
      if (r.state == ResourceState::Ready) {
        request->receipt.state = RequestState::Ready;
        request->receipt.resource = *r.handle;
        request->retained = r.value;
      } else if (r.state == ResourceState::Failed || r.state == ResourceState::Cancelled) {
        request->receipt.state =
            r.state == ResourceState::Cancelled ? RequestState::Cancelled : RequestState::Failed;
        request->receipt.diagnostic = r.diagnostic;
      }
    }
    wanted = s.needed();
    for (std::size_t i = 0; i < s.records.size(); ++i) {
      auto &r = s.records[i];
      if (wanted[i] && !s.stopping)
        continue;
      if (r.load)
        continue;
      if (r.handle) {
        auto erased = s.resources.erase(*r.handle);
        if (!erased)
          return erased;
        r.handle.reset();
      }
      r.value.reset();
      if (r.state == ResourceState::Ready)
        r.state = ResourceState::Retiring;
    }
    s.counters.drain_nanoseconds += elapsed(start);
    return {};
  } catch (const std::bad_alloc &) {
    s.counters.drain_nanoseconds += elapsed(start);
    return fail(ErrorCode::Exhausted, "asset drain allocation; owners retained");
  }
}
void Service::request_stop() noexcept {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  s.stopping = true;
  for (auto &[h, r] : s.subscribers) {
    (void)h;
    if (r->receipt.state == RequestState::Pending) {
      r->receipt.state = RequestState::Cancelled;
      r->receipt.diagnostic = {s.catalog.entries[r->index].id, {}, Stage::Stop,
                               ErrorCode::Cancelled,           {}, 0};
    }
  }
  s.changed.notify_all();
}
Result<void> Service::stop(std::chrono::milliseconds timeout) {
  auto &s = *state_;
  if (s.owner != std::this_thread::get_id())
    return fail(ErrorCode::WrongThread, "asset stop owner required");
  if (timeout.count() < 0)
    return fail(ErrorCode::InvalidInput, "negative asset timeout");
  request_stop();
  auto start = Clock::now();
  for (;;) {
    auto drained = drain();
    if (!drained)
      return drained;
    bool done;
    {
      std::lock_guard lock(s.mutex);
      done = s.io_done && std::none_of(s.records.begin(), s.records.end(),
                                       [](const Record &r) { return bool(r.load); });
    }
    if (done)
      break;
    if (timeout != std::chrono::milliseconds::max() && Clock::now() - start >= timeout)
      return fail(ErrorCode::Timeout, "asset owners retained until quiescence");
    std::this_thread::yield();
  }
  if (io_) {
    auto joined = io_->join();
    if (!joined)
      return joined;
    io_.reset();
  }
  std::lock_guard lock(s.mutex);
  for (auto &[h, r] : s.subscribers) {
    (void)r;
    auto erased = s.requests.erase(h);
    if (!erased)
      return erased;
  }
  s.subscribers.clear();
  return {};
}
Stats Service::stats() const {
  auto &s = *state_;
  std::lock_guard lock(s.mutex);
  auto result = s.counters;
  result.requests = s.subscribers.size();
  for (const auto &r : s.records) {
    if (r.state == ResourceState::Queued)
      ++result.queue_depth;
    if (r.load)
      ++result.in_flight;
    if (r.load && r.load->group)
      ++result.decode_jobs;
  }
  std::lock_guard account(s.accounting->mutex);
  result.payload_bytes = s.accounting->bytes;
  result.peak_payload_bytes = s.accounting->peak;
  result.temporary_bytes = s.accounting->temporary;
  result.peak_temporary_bytes = s.accounting->peak_temporary;
  result.retained_revisions = s.accounting->revisions;
  result.peak_retained_revisions = s.accounting->peak_revisions;
  return result;
}
} // namespace race::assets
