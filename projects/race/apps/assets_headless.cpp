#include "assets_module.hpp"
#include "race/assets/assets.hpp"
#include "race/build_identity.hpp"
#include "race/runtime/lifecycle.hpp"
#include "race/scene/scene.hpp"
#include <cstdio>
#include <thread>
using namespace race;
using namespace std::chrono_literals;
namespace {
Result<void> run(const char *root, assets::AssetId id) {
  auto platform = platform::make_services();
  if (!platform)
    return platform.error();
  auto files = platform::make_rooted_files(root);
  if (!files)
    return files.error();
  auto bytes = files.value()->read("catalog.bin", assets::Limits{}.catalog_bytes);
  if (!bytes)
    return bytes.error();
  auto catalog = assets::decode_catalog(bytes.value());
  if (!catalog)
    return catalog.error();
  auto memory = TrackedAllocator::create({64 * 1024 * 1024});
  if (!memory)
    return memory.error();
  auto jobs = core::Scheduler::create(*platform.value().threads, {128, 2, 16});
  if (!jobs)
    return jobs.error();
  core::Diagnostics diagnostics(*platform.value().clock);
  runtime::Runtime runtime(memory.value(), *jobs.value(), *platform.value().clock, diagnostics);
  auto module = std::make_unique<fixtures::AssetsModule>(files.value(), *platform.value().threads,
                                                         std::move(catalog.value()));
  auto *adapter = module.get();
  auto added = runtime.add(std::move(module));
  if (!added)
    return added;
  auto started = runtime.start();
  if (!started)
    return started;
  auto schema = scene::Schemas::create(
      {{2, 1, "assetfixture", {{1, "asset", PersistentId{}}, {2, "resolved", std::int64_t{0}}}}});
  if (!schema)
    return schema.error();
  scene::Limits limits;
  limits.objects = 8;
  limits.template_objects = 8;
  limits.components = 16;
  limits.history_groups = 4;
  auto one = scene::World::create(memory.value(), schema.value(), limits),
       two = scene::World::create(memory.value(), schema.value(), limits);
  if (!one)
    return one.error();
  if (!two)
    return two.error();
  for (auto *world : {one.value().get(), two.value().get()}) {
    auto made = world->apply(
        {{scene::Command::create({{10, 1}, {}, {{2, 1, {{1, id}, {2, std::int64_t{0}}}}}, {}})}});
    if (!made)
      return made;
  }
  auto view = one.value()->read();
  if (!view)
    return view.error();
  auto object = view.value().find({10, 1});
  if (!object)
    return object.error();
  auto asset = view.value().value(object.value(), 2, 1);
  if (!asset)
    return asset.error();
  if (adapter->service->stats().reads)
    return fail(ErrorCode::Invariant, "Scene lookup performed hidden I/O");
  auto request = adapter->service->request(std::get<PersistentId>(asset.value()));
  if (!request)
    return request.error();
  auto request2 = adapter->service->request(id);
  if (!request2)
    return request2.error();
  auto start = std::chrono::steady_clock::now();
  assets::Receipt receipt;
  for (;;) {
    auto drain = adapter->service->drain();
    if (!drain)
      return drain;
    auto result = adapter->service->poll(request.value());
    if (!result)
      return result.error();
    receipt = result.value();
    if (receipt.state != assets::RequestState::Pending)
      break;
    if (std::chrono::steady_clock::now() - start > 10s)
      return fail(ErrorCode::Timeout, "headless watchdog");
    std::this_thread::yield();
  }
  if (receipt.state != assets::RequestState::Ready)
    return fail(ErrorCode::Invariant, "headless asset failed");
  auto lease = adapter->service->acquire(receipt.resource);
  if (!lease)
    return lease.error();
  auto other = adapter->service->poll(request2.value());
  if (!other || other.value().resource != receipt.resource)
    return fail(ErrorCode::Invariant, "cross World load not coalesced");
  // Explicit bridge copies a derived fixture value into a World command. The lease
  // remains immutable; its data is never used as shared mutable Scene storage.
  auto ticket = one.value()->enqueue(
      {{scene::Command::set({10, 1}, 2, 2, std::int64_t(lease.value()->values().size()))}});
  if (!ticket)
    return ticket.error();
  auto blocked = one.value()->publish_pending();
  if (blocked || blocked.error().code != ErrorCode::Busy)
    return fail(ErrorCode::Invariant, "ReadView must block publication");
  view.value().release();
  auto published = one.value()->publish_pending();
  if (!published)
    return published;
  auto completion = one.value()->take_completion(ticket.value());
  if (!completion)
    return completion.error();
  if (!completion.value().result)
    return completion.value().result;
  auto isolated = two.value()->read();
  if (!isolated)
    return isolated.error();
  auto h = isolated.value().find({10, 1});
  if (!h)
    return h.error();
  auto v = isolated.value().value(h.value(), 2, 2);
  if (!v || std::get<std::int64_t>(v.value()) != 0)
    return fail(ErrorCode::Invariant, "Worlds shared mutable asset result");
  isolated.value().release();
  auto stats = adapter->service->stats();
  auto rss = platform::memory_info();
  std::printf("{\"source\":\"%s\",\"authored_sha256\":\"%s\",\"scene_explicit_resolution\":true,"
              "\"world_isolation\":true,\"reads\":%llu,\"decodes\":%llu,\"payload_bytes\":%zu,"
              "\"temporary_peak_bytes\":%zu,\"bounded_metadata_capacity_bytes\":%zu,\"rss_bytes\":%"
              "llu,\"read_ns\":%llu,\"decode_ns\":%llu,\"drain_ns\":%llu}\n",
              build::source_commit, build::authored_input_sha256, (unsigned long long)stats.reads,
              (unsigned long long)stats.decodes, stats.payload_bytes, stats.peak_temporary_bytes,
              stats.bounded_metadata_capacity_bytes,
              (unsigned long long)(rss ? rss.value().resident_bytes : 0),
              (unsigned long long)stats.read_nanoseconds,
              (unsigned long long)stats.decode_nanoseconds,
              (unsigned long long)stats.drain_nanoseconds);
  auto stopped = runtime.stop(10s);
  if (!stopped)
    return stopped;
  if (lease.value()->values().empty())
    return fail(ErrorCode::Invariant, "lease lost after consumer destruction");
  return {};
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  auto id = assets::parse_id(argv[2]);
  if (!id)
    return 2;
  auto result = run(argv[1], id.value());
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().message);
    return 1;
  }
  return 0;
}
