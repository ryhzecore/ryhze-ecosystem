#include "assets_support.hpp"
#include <limits>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#include <crtdbg.h>
#include <exception>
#endif
namespace {
thread_local std::size_t failure = SIZE_MAX;
std::atomic<std::size_t> gate_bytes{0}, fail_payload{0};
std::atomic<bool> gate_entered{false}, gate_open{true};
void point() {
  if (failure == SIZE_MAX)
    return;
  if (!failure) {
    failure = SIZE_MAX;
    throw std::bad_alloc();
  }
  --failure;
}
} // namespace
#ifdef _WIN32
void* operator new(std::size_t n) { point(); if(auto p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
#else
extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__real__Znam(std::size_t);
extern "C" void *__wrap__Znwm(std::size_t n) {
  point();
  return __real__Znwm(n);
}
extern "C" void *__wrap__Znam(std::size_t n) {
  point();
  return __real__Znam(n);
}
extern "C" void *__real__ZnwmSt11align_val_tRKSt9nothrow_t(std::size_t, std::align_val_t,
                                                           const std::nothrow_t &);
#endif
#ifdef _WIN32
void* operator new(std::size_t n, std::align_val_t a,
#else
extern "C" void *__wrap__ZnwmSt11align_val_tRKSt9nothrow_t(std::size_t n, std::align_val_t a,
#endif
                                                           const std::nothrow_t &t) noexcept {
  if (fail_payload.load() == n) {
    std::size_t expected = n;
    if (fail_payload.compare_exchange_strong(expected, 0))
      return nullptr;
  }
  if (gate_bytes.load() == n) {
    std::size_t expected = n;
    if (gate_bytes.compare_exchange_strong(expected, 0)) {
      gate_entered.store(true);
      gate_entered.notify_all();
      while (!gate_open.load())
        gate_open.wait(false);
    }
  }
#ifdef _WIN32
  (void)t;
  return _aligned_malloc(n?n:1,static_cast<std::size_t>(a));
#else
  return __real__ZnwmSt11align_val_tRKSt9nothrow_t(n, a, t);
#endif
}
#ifdef _WIN32
void operator delete(void* p,std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { _aligned_free(p); }
void operator delete(void* p,std::align_val_t,const std::nothrow_t&) noexcept { _aligned_free(p); }
#endif
void asset_fail_payload_once(std::size_t n) { fail_payload = n; }
void asset_fail_after(std::size_t n) { failure = n; }
void asset_fail_disable() { failure = SIZE_MAX; }
void asset_decode_gate_arm(std::size_t n) {
  gate_entered = false;
  gate_open = false;
  gate_bytes = n;
}
bool asset_decode_gate_entered() { return gate_entered.load(); }
void asset_decode_gate_open() {
  gate_open = true;
  gate_open.notify_all();
}
void asset_fault_tests() {
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  std::set_terminate([] {
    std::fprintf(stderr, "faults: std::terminate\n");
    if (auto exception = std::current_exception()) {
      try { std::rethrow_exception(exception); }
      catch (const std::exception& e) { std::fprintf(stderr, "faults: exception %s\n", e.what()); }
      catch (...) { std::fprintf(stderr, "faults: unknown exception\n"); }
    }
    std::_Exit(86);
  });
#endif
  FakeFiles files;
  auto c = cook(diamond(), files);
  CHECK(c);
  auto encoded = encode_catalog(c.value());
  CHECK(encoded);
  unsigned failed = 0, passed = 0;
  for (std::size_t n = 0; n < 128; ++n) {
    asset_fail_after(n);
    auto r = decode_catalog(encoded.value());
    asset_fail_disable();
    if (r)
      ++passed;
    else {
      CHECK(r.error().code == ErrorCode::Exhausted);
      ++failed;
    }
  }
  CHECK(failed && passed);
  for (std::size_t n = 0; n < 16; ++n) {
    asset_fail_after(n);
    auto r = validate_catalog(c.value(), {});
    asset_fail_disable();
    CHECK(r || r.error().code == ErrorCode::Exhausted);
  }
  Fixture f({{aid(1), 1, 16, 0, 1, {}, {}}});
  for (std::size_t n = 0; n < 48; ++n) {
    asset_fail_after(n);
    auto r = Service::create(f.catalog, {}, f.memory, f.files, *f.platform.threads, *f.jobs);
    asset_fail_disable();
    if (r) {
      CHECK(r.value()->stop(10s));
      ++passed;
    } else {
      CHECK(r.error().code == ErrorCode::Exhausted);
      ++failed;
    }
  }
  struct ThrowThreads : platform::Threads {
    Result<std::unique_ptr<platform::Thread>> start(std::function<void()>) override {
      throw std::bad_alloc();
    }
  } throwing;
  CHECK_ERROR(Service::create(f.catalog, {}, f.memory, f.files, throwing, *f.jobs),
              ErrorCode::Exhausted);
  for (std::size_t n = 0; n < 16; ++n) {
    asset_fail_after(n);
    auto r = f.service->request(aid(1));
    asset_fail_disable();
    if (r) {
      CHECK(f.wait(r.value()).state == RequestState::Ready);
      CHECK(f.service->release(r.value()));
      CHECK(f.service->drain());
    } else {
      CHECK(r.error().code == ErrorCode::Exhausted);
      CHECK(f.service->stats().requests == 0);
    }
  }
  // Allocation fails while releasing terminal Scheduler ownership. The same
  // request remains valid, and the next drain can finish without losing context.
  asset_decode_gate_arm(16 * 8);
  auto h = f.service->request(aid(1));
  CHECK(h);
  auto start = std::chrono::steady_clock::now();
  while (!asset_decode_gate_entered()) {
    CHECK(f.service->drain());
    CHECK(std::chrono::steady_clock::now() - start < 10s);
  }
  asset_decode_gate_open();
  while (f.jobs->stats().running || f.jobs->stats().pending) {
    CHECK(std::chrono::steady_clock::now() - start < 10s);
    std::this_thread::yield();
  }
  asset_fail_after(0);
  auto failed_release = f.service->drain();
  asset_fail_disable();
  CHECK_ERROR(failed_release, ErrorCode::Exhausted);
  CHECK(f.jobs->stats().occupied == 1);
  CHECK(f.wait(h.value()).state == RequestState::Ready);
  CHECK(f.service->release(h.value()));
  f.empty();
  CHECK(f.memory.snapshot().total.live == 0);
  std::printf("faults: %u injected failures, %u completed paths; startup rollback and terminal "
              "release retry passed\n",
              failed, passed);
}
