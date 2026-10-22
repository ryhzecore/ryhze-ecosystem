#include "../apps/assets_module.hpp"
#include "assets_support.hpp"
namespace {
void wait_decode(Fixture &f) {
  auto start = std::chrono::steady_clock::now();
  while (!asset_decode_gate_entered()) {
    CHECK(f.service->drain());
    CHECK(std::chrono::steady_clock::now() - start < 10s);
    std::this_thread::yield();
  }
}
} // namespace
void asset_runtime_tests() {
  {
    Fixture f;
    std::array<RequestHandle, 8> requests{};
    std::array<std::thread, 8> callers;
    for (std::size_t i = 0; i < callers.size(); ++i)
      callers[i] = std::thread([&, i] {
        auto r = f.service->request(aid(1));
        CHECK(r);
        requests[i] = r.value();
      });
    for (auto &t : callers)
      t.join();
    auto r = f.wait(requests[0]);
    CHECK(r.state == RequestState::Ready);
    for (auto h : requests)
      CHECK(f.wait(h).resource == r.resource);
    CHECK(f.files->read_count() == 4);
    CHECK(f.service->stats().decodes == 4);
    CHECK(f.service->stats().publications == 4);
    auto lease = f.service->acquire(r.resource);
    CHECK(lease);
    CHECK(lease.value()->values()[0] == fixture_value(1, 0));
    CHECK_ERROR(f.service->request({}), ErrorCode::InvalidInput);
    CHECK_ERROR(f.service->request(aid(99)), ErrorCode::NotFound);
    CHECK_ERROR(f.service->request(aid(1), 2), ErrorCode::InvalidInput);
    CHECK_ERROR(f.service->acquire(r.resource, 2), ErrorCode::InvalidInput);
    Fixture foreign;
    CHECK_ERROR(foreign.service->acquire(r.resource), ErrorCode::StaleHandle);
    CHECK_ERROR(foreign.service->poll(requests[0]), ErrorCode::StaleHandle);
    auto same = f.service->request(aid(1));
    CHECK(same);
    CHECK_ERROR(f.service->retire(r.resource), ErrorCode::Busy);
    CHECK(f.wait(same.value()).state == RequestState::Ready);
    CHECK(f.service->release(same.value()));
    CHECK(f.service->retire(r.resource));
    CHECK_ERROR(f.service->acquire(r.resource), ErrorCode::StaleHandle);
    CHECK(lease.value()->values().size() == 32);
    for (auto h : requests)
      CHECK(f.service->release(h));
    CHECK_ERROR(f.service->poll(requests[0]), ErrorCode::StaleHandle);
    auto retry = f.service->request(aid(1));
    CHECK(retry);
    auto fresh = f.wait(retry.value());
    CHECK(fresh.state == RequestState::Ready);
    CHECK(fresh.resource != r.resource);
    CHECK(f.service->release(retry.value()));
    f.empty();
    f.service.reset();
    CHECK(lease.value()->values()[2] == fixture_value(1, 2));
    lease = fail(ErrorCode::Cancelled, "release test lease");
    CHECK(f.memory.snapshot().total.live == 0);
  }
  {
    // One cancelled subscriber cannot cancel another; no callbacks under lock.
    Fixture f({{aid(1), 7, 64, 0, 1, {}, {}}});
    auto gate = std::make_shared<Gate>();
    f.files->gate = gate;
    f.files->gated = f.catalog.entries[0].location;
    auto a = f.service->request(aid(1));
    CHECK(a);
    gate->wait();
    auto b = f.service->request(aid(1));
    CHECK(b);
    CHECK(f.service->cancel(a.value()));
    CHECK(f.service->poll(a.value()).value().state == RequestState::Cancelled);
    gate->release();
    auto r = f.wait(b.value());
    CHECK(r.state == RequestState::Ready);
    CHECK(f.files->read_count() == 1);
    CHECK(f.service->poll(a.value()).value().state == RequestState::Cancelled);
    CHECK(f.service->release(a.value()));
    CHECK(f.service->release(b.value()));
    f.empty();
    CHECK(f.memory.snapshot().total.live == 0);
  }
  {
    // Cancel before start while the sole I/O adapter is gated in another read.
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}, {aid(2), 2, 64, 0, 1, {}, {}}});
    auto gate = std::make_shared<Gate>();
    f.files->gate = gate;
    f.files->gated = f.catalog.entries[0].location;
    auto first = f.service->request(aid(1));
    CHECK(first);
    gate->wait();
    auto second = f.service->request(aid(2));
    CHECK(second);
    CHECK(f.service->cancel(second.value()));
    CHECK(f.service->drain());
    CHECK(f.service->poll(second.value()).value().state == RequestState::Cancelled);
    gate->release();
    CHECK(f.wait(first.value()).state == RequestState::Ready);
    CHECK(f.files->read_count() == 1);
    CHECK(f.service->release(first.value()));
    CHECK(f.service->release(second.value()));
    f.empty();
  }
  {
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}});
    auto gate = std::make_shared<Gate>();
    f.files->gate = gate;
    f.files->gated = f.catalog.entries[0].location;
    auto h = f.service->request(aid(1));
    CHECK(h);
    gate->wait();
    auto retained = f.service->stats().temporary_bytes;
    CHECK(retained > 0);
    CHECK_ERROR(f.service->stop(0ms), ErrorCode::Timeout);
    CHECK(f.service->stats().temporary_bytes == retained);
    CHECK_ERROR(f.service->request(aid(1)), ErrorCode::Stopping);
    CHECK(f.service->poll(h.value()).value().state == RequestState::Cancelled);
    gate->release();
    f.empty();
    CHECK(f.memory.snapshot().total.live == 0);
  }
  {
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}});
    auto gate = std::make_shared<Gate>();
    f.files->gate = gate;
    f.files->gated = f.catalog.entries[0].location;
    auto h = f.service->request(aid(1));
    CHECK(h);
    gate->wait();
    CHECK(f.service->release(h.value()));
    CHECK(f.service->drain());
    CHECK_ERROR(f.service->request(aid(1)), ErrorCode::Busy);
    gate->release();
    auto start = std::chrono::steady_clock::now();
    while (f.service->stats().in_flight) {
      CHECK(f.service->drain());
      CHECK(std::chrono::steady_clock::now() - start < 10s);
    }
    CHECK_ERROR(f.service->poll(h.value()), ErrorCode::StaleHandle);
    auto retry = f.service->request(aid(1));
    CHECK(retry);
    CHECK(f.wait(retry.value()).state == RequestState::Ready);
    CHECK(f.service->release(retry.value()));
    f.empty();
  }
  {
    // The allocator wrapper gates inside the CPU decode callback, not a clock.
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}});
    asset_decode_gate_arm(64 * 8);
    auto h = f.service->request(aid(1));
    CHECK(h);
    wait_decode(f);
    CHECK(f.service->cancel(h.value()));
    CHECK(f.service->drain());
    CHECK_ERROR(f.service->request(aid(1)), ErrorCode::Busy);
    CHECK_ERROR(f.service->stop(0ms), ErrorCode::Timeout);
    CHECK(f.service->stats().in_flight == 1);
    asset_decode_gate_open();
    f.empty();
    CHECK(f.memory.snapshot().total.live == 0);
  }
  {
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}});
    asset_decode_gate_arm(64 * 8);
    auto a = f.service->request(aid(1)), b = f.service->request(aid(1));
    CHECK(a && b);
    wait_decode(f);
    CHECK(f.service->release(a.value()));
    CHECK(f.service->drain());
    asset_decode_gate_open();
    CHECK(f.wait(b.value()).state == RequestState::Ready);
    CHECK_ERROR(f.service->poll(a.value()), ErrorCode::StaleHandle);
    CHECK(f.service->release(b.value()));
    f.empty();
  }
  {
    Fixture f;
    auto dep = f.service->request(aid(4));
    CHECK(dep);
    auto rd = f.wait(dep.value());
    CHECK(rd.state == RequestState::Ready);
    auto parent = f.service->request(aid(1));
    CHECK(parent);
    CHECK_ERROR(f.service->retire(rd.resource), ErrorCode::Busy);
    CHECK(f.wait(parent.value()).state == RequestState::Ready);
    CHECK(f.service->release(dep.value()));
    CHECK(f.service->release(parent.value()));
    f.empty();
  }
  {
    Fixture f;
    {
      std::lock_guard lock(f.files->mutex);
      auto &bad = f.files->data[f.catalog.entries[3].location];
      bad.back() ^= std::byte{1};
    }
    auto h = f.service->request(aid(1));
    CHECK(h);
    auto r = f.wait(h.value());
    CHECK(r.state == RequestState::Failed);
    CHECK(r.diagnostic.code == ErrorCode::FailedPrerequisite);
    CHECK(r.diagnostic.cause == aid(4));
    CHECK(f.service->release(h.value()));
    auto unwind_start = std::chrono::steady_clock::now();
    while (f.service->stats().in_flight) {
      CHECK(f.service->drain());
      CHECK(std::chrono::steady_clock::now() - unwind_start < 10s);
    }
    CHECK(f.service->drain());
    CHECK(f.memory.snapshot().total.live == 0);
    CHECK(f.service->stats().temporary_bytes == 0);
    f.empty();
    CHECK(f.memory.snapshot().total.live == 0);
    CHECK(f.service->stats().failures > 0);
  }
  {
    Limits l;
    l.requests = 1;
    l.io_queue = 1;
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}, {aid(2), 2, 64, 0, 1, {}, {}}}, l);
    auto h = f.service->request(aid(1));
    CHECK(h);
    CHECK_ERROR(f.service->request(aid(2)), ErrorCode::QueueFull);
    CHECK(f.wait(h.value()).state == RequestState::Ready);
    CHECK(f.service->release(h.value()));
    f.empty();
    Fixture graph(diamond(), l);
    CHECK_ERROR(graph.service->request(aid(1)), ErrorCode::QueueFull);
    CHECK(graph.service->stats().requests == 0);
    CHECK(graph.files->read_count() == 0);
  }
  {
    Limits l;
    l.temporary_bytes = 128;
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}}, l);
    auto h = f.service->request(aid(1));
    CHECK(h);
    CHECK(f.wait(h.value()).state == RequestState::Failed);
    CHECK(f.service->stats().temporary_bytes == 0);
    CHECK(f.service->release(h.value()));
    f.empty();
    l = {};
    l.retained_bytes = 128;
    Fixture bytes({{aid(1), 1, 64, 0, 1, {}, {}}}, l);
    h = bytes.service->request(aid(1));
    CHECK(h);
    CHECK(bytes.wait(h.value()).state == RequestState::Failed);
    CHECK(bytes.service->release(h.value()));
    bytes.empty();
    CHECK(bytes.memory.snapshot().total.live == 0);
  }
  {
    Limits l;
    l.retained_revisions = 1;
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}}, l);
    auto a = f.service->request(aid(1));
    CHECK(a);
    auto r = f.wait(a.value());
    auto lease = f.service->acquire(r.resource);
    CHECK(lease);
    CHECK(f.service->retire(r.resource));
    CHECK(f.service->release(a.value()));
    auto b = f.service->request(aid(1));
    CHECK(b);
    CHECK(f.wait(b.value()).state == RequestState::Failed);
    CHECK(f.service->release(b.value()));
    lease = fail(ErrorCode::Cancelled, "drop retained revision");
    auto c = f.service->request(aid(1));
    CHECK(c);
    CHECK(f.wait(c.value()).state == RequestState::Ready);
    CHECK(f.service->release(c.value()));
    f.empty();
    CHECK(f.service->stats().peak_retained_revisions == 1);
  }
  {
    Fixture f({{aid(1), 1, 16, 0, 1, {}, {}}});
    for (int i = 0; i < 80; ++i) {
      auto h = f.service->request(aid(1));
      CHECK(h);
      CHECK(f.wait(h.value()).state == RequestState::Ready);
      CHECK(f.service->release(h.value()));
      CHECK(f.service->drain());
      CHECK(f.jobs->stats().occupied == 0);
    }
    CHECK(f.service->stats().decodes == 80);
    f.empty();
    CHECK(f.memory.snapshot().total.live == 0);
  }
  {
    Fixture f({{aid(1), 1, 16, 0, 1, {}, {}}});
    {
      std::lock_guard lock(f.files->mutex);
      f.files->throw_read = true;
    }
    auto h = f.service->request(aid(1));
    CHECK(h);
    auto r = f.wait(h.value());
    CHECK(r.state == RequestState::Failed && r.diagnostic.code == ErrorCode::Exhausted);
    CHECK(f.service->release(h.value()));
    f.empty();
    std::thread wrong([&] {
      CHECK_ERROR(f.service->drain(), ErrorCode::WrongThread);
      CHECK_ERROR(f.service->stop(0ms), ErrorCode::WrongThread);
    });
    wrong.join();
  }

  {
    Fixture f({{aid(1), 1, 64, 0, 1, {}, {}}});
    asset_fail_payload_once(512);
    auto h = f.service->request(aid(1));
    CHECK(h);
    auto receipt = f.wait(h.value());
    CHECK(receipt.state == RequestState::Failed);
    CHECK(receipt.diagnostic.code == ErrorCode::Exhausted);
    CHECK(f.service->release(h.value()));
    CHECK(f.service->drain());
    CHECK(f.memory.snapshot().total.live == 0);
    CHECK(f.service->stats().temporary_bytes == 0);
    auto retry = f.service->request(aid(1));
    CHECK(retry);
    CHECK(f.wait(retry.value()).state == RequestState::Ready);
    CHECK(f.service->release(retry.value()));
    CHECK(f.service->drain());
    CHECK(f.memory.snapshot().total.live == 0);
    CHECK(f.service->stats().retained_revisions == 0);
  }
  {
    Fixture f({{aid(1), 1, 16, 0, 1, {}, {}}});
    std::vector<core::GroupHandle> groups;
    for (int i = 0; i < 16; ++i) {
      auto g = f.jobs->create_group();
      CHECK(g);
      groups.push_back(g.value());
    }
    auto h = f.service->request(aid(1));
    CHECK(h);
    auto failed = f.wait(h.value());
    CHECK(failed.state == RequestState::Failed);
    CHECK(failed.diagnostic.stage == Stage::Decode);
    CHECK(f.service->release(h.value()));
    for (auto g : groups) {
      CHECK(f.jobs->close(g));
      CHECK(f.jobs->release(g));
    }
    CHECK(f.jobs->stats().occupied == 0);
    auto group = f.jobs->create_group();
    CHECK(group);
    for (int i = 0; i < 32; ++i) {
      core::JobDesc d;
      d.function = [](const core::JobContext &) -> Result<void> { return {}; };
      CHECK(f.jobs->submit(group.value(), d));
    }
    h = f.service->request(aid(1));
    CHECK(h);
    failed = f.wait(h.value());
    CHECK(failed.state == RequestState::Failed && failed.diagnostic.stage == Stage::Decode);
    CHECK(f.service->release(h.value()));
    CHECK(f.jobs->close(group.value()));
    CHECK(f.jobs->wait(group.value(), 10s));
    CHECK(f.jobs->release(group.value()));
    CHECK(f.service->drain());
    CHECK(f.memory.snapshot().total.live == 0);
  }
  {
    Fixture f({{aid(1), 1, 16, 0, 1, {}, {}}});
    f.empty();
    f.service.reset();
    core::Diagnostics diagnostics(*f.platform.clock);
    runtime::Runtime runtime(f.memory, *f.jobs, *f.platform.clock, diagnostics);
    auto module = std::make_unique<fixtures::AssetsModule>(f.files, *f.platform.threads, f.catalog);
    auto *owner = module.get();
    CHECK(runtime.add(std::move(module)));
    CHECK(runtime.start());
    auto gate = std::make_shared<Gate>();
    f.files->gate = gate;
    f.files->gated = f.catalog.entries[0].location;
    auto h = owner->service->request(aid(1));
    CHECK(h);
    gate->wait();
    CHECK_ERROR(runtime.stop(0ms), ErrorCode::Timeout);
    CHECK(owner->service);
    CHECK(owner->service->stats().temporary_bytes > 0);
    gate->release();
    CHECK(runtime.stop(10s));
    CHECK(!owner->service);
    CHECK(f.memory.snapshot().total.live == 0);
    CHECK(f.jobs->stats().occupied == 0);
  }
  {
    Fixture f({{aid(1), 1, 16, 0, 1, {}, {}}});
    f.empty();
    f.service.reset();
    struct ThrowThreads : platform::Threads {
      Result<std::unique_ptr<platform::Thread>> start(std::function<void()>) override {
        throw std::bad_alloc();
      }
    } throwing;
    core::Diagnostics diagnostics(*f.platform.clock);
    runtime::Runtime runtime(f.memory, *f.jobs, *f.platform.clock, diagnostics);
    auto module = std::make_unique<fixtures::AssetsModule>(f.files, throwing, f.catalog);
    auto *owner = module.get();
    CHECK(runtime.add(std::move(module)));
    CHECK_ERROR(runtime.start(), ErrorCode::Exhausted);
    CHECK(runtime.stop(10s));
    CHECK(!owner->service);
    CHECK(f.jobs->stats().occupied == 0);
    CHECK(f.memory.snapshot().total.live == 0);
  }
  std::printf(
      "runtime: concurrent coalescing/diamond; cancellation at queue/read/decode; stale retry; "
      "timeout retention; leases; dependency failure; bounded memory; group recycling passed\n");
}
