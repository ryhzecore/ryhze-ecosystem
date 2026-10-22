#include "../apps/desktop_profile.hpp"
#include "test.hpp"
#include <atomic>
#include <limits>
#include <new>
#include <thread>

#if !defined(RACE_DESKTOP_ALLOCATION_PROFILE) || !RACE_DESKTOP_ALLOCATION_PROFILE || !defined(RACE_DESKTOP_PROFILE_TESTING) || !RACE_DESKTOP_PROFILE_TESTING
#error Build this standalone test and desktop_profile.cpp with both profiler/test feature definitions enabled.
#endif
using namespace race::desktop::profile;
namespace {
void expect(std::uint64_t count,std::uint64_t bytes,bool overflow=false) {
  const auto value=allocation_snapshot();
  CHECK(value.requests==count); CHECK(value.requested_bytes==bytes); CHECK(value.overflowed==overflow);
}
void disabled_and_nested_scopes() {
  CHECK(allocation_counting_available()); CHECK(reset_allocation_counters());
  ::operator delete(::operator new(12)); expect(0,0);
  { AllocationScope disabled(false); CHECK(!disabled.active()); ::operator delete(::operator new(7)); }
  expect(0,0);
  AllocationScope outer; CHECK(outer.active());
  ::operator delete(::operator new(3));
  { AllocationScope inner; ::operator delete[](::operator new[](5)); CHECK(!reset_allocation_counters()); }
  ::operator delete(::operator new(7)); expect(3,15);
  outer.stop(); outer.stop(); CHECK(!outer.active());
  ::operator delete(::operator new(9)); expect(3,15);
  CHECK(reset_allocation_counters()); expect(0,0);
}
void overloads_and_aligned_pairs() {
  CHECK(reset_allocation_counters()); AllocationScope scope;
  void* a=::operator new(0); CHECK(a); ::operator delete(a);
  a=::operator new[](2); CHECK(a); ::operator delete[](a,std::size_t{2});
  a=::operator new(3,std::nothrow); CHECK(a); ::operator delete(a,std::nothrow);
  a=::operator new[](4,std::nothrow); CHECK(a); ::operator delete[](a,std::nothrow);
  constexpr auto alignment=std::align_val_t{64};
  a=::operator new(5,alignment); CHECK(a); CHECK(reinterpret_cast<std::uintptr_t>(a)%64==0); ::operator delete(a,alignment);
  a=::operator new[](6,alignment); CHECK(a); CHECK(reinterpret_cast<std::uintptr_t>(a)%64==0); ::operator delete[](a,std::size_t{6},alignment);
  a=::operator new(7,alignment,std::nothrow); CHECK(a); CHECK(reinterpret_cast<std::uintptr_t>(a)%64==0); ::operator delete(a,alignment,std::nothrow);
  a=::operator new[](8,alignment,std::nothrow); CHECK(a); CHECK(reinterpret_cast<std::uintptr_t>(a)%64==0); ::operator delete[](a,alignment,std::nothrow);
  expect(8,35);
  ::operator delete(nullptr); ::operator delete[](nullptr);
  ::operator delete(nullptr,alignment); ::operator delete[](nullptr,alignment);
  expect(8,35);
}
unsigned handler_calls{};
void retry_handler() { ++handler_calls; }
void throwing_handler() { ++handler_calls; throw std::bad_alloc{}; }
void deterministic_failures_and_handlers() {
  CHECK(reset_allocation_counters()); AllocationScope scope;
  const auto original=std::set_new_handler(nullptr);
  testing::fail_next_allocation_attempts(1);
  bool threw=false;
  try { void* p=::operator new(11); ::operator delete(p); } catch(const std::bad_alloc&) { threw=true; }
  CHECK(threw); expect(1,11);
  testing::fail_next_allocation_attempts(1);
  CHECK(::operator new[](13,std::nothrow)==nullptr); expect(2,24);
  handler_calls=0; std::set_new_handler(retry_handler);
  testing::fail_next_allocation_attempts(1);
  void* p=::operator new(17); CHECK(p); ::operator delete(p);
  CHECK(handler_calls==1); expect(3,41); // Retry is one request, not two.
  std::set_new_handler(throwing_handler); testing::fail_next_allocation_attempts(1);
  CHECK(::operator new(19,std::align_val_t{128},std::nothrow)==nullptr);
  CHECK(handler_calls==2); expect(4,60);
  testing::fail_next_allocation_attempts(0); std::set_new_handler(original);
}
void saturating_counters() {
  constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
  CHECK(testing::seed_counters(maximum-1,maximum-3));
  { AllocationScope scope;
    ::operator delete(::operator new(3)); expect(maximum,maximum);
    ::operator delete(::operator new(1)); expect(maximum,maximum,true);
    CHECK(!testing::seed_counters(0,0));
  }
  CHECK(reset_allocation_counters()); expect(0,0);
}
void concurrent_request_totals() {
  std::atomic<unsigned> ready{},done{}; std::atomic<bool> go{},leave{};
  auto work=[&](std::size_t size) {
    ready.fetch_add(1,std::memory_order_release);
    while(!go.load(std::memory_order_acquire)) {}
    for(unsigned i=0;i<50;++i) ::operator delete(::operator new(size));
    done.fetch_add(1,std::memory_order_release);
    while(!leave.load(std::memory_order_acquire)) {}
  };
  std::thread first(work,11),second(work,13);
  while(ready.load(std::memory_order_acquire)!=2) {}
  CHECK(reset_allocation_counters());
  { AllocationScope scope; go.store(true,std::memory_order_release);
    while(done.load(std::memory_order_acquire)!=2) {}
    expect(100,1200);
  }
  leave.store(true,std::memory_order_release); first.join(); second.join();
  expect(100,1200);
}
}
int main() {
  disabled_and_nested_scopes(); overloads_and_aligned_pairs();
  deterministic_failures_and_handlers(); saturating_counters(); concurrent_request_totals();
  std::puts("desktop allocation profiler: disabled/scoped, overloads/alignment, deterministic OOM/handler, saturation and concurrent counts passed");
}
