#include "desktop_profile.hpp"

#ifndef RACE_DESKTOP_ALLOCATION_PROFILE
#define RACE_DESKTOP_ALLOCATION_PROFILE 0
#endif

#if RACE_DESKTOP_ALLOCATION_PROFILE
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
struct Counters {
  std::atomic_flag lock=ATOMIC_FLAG_INIT;
  std::atomic<bool> enabled{false};
  std::uint64_t scopes{}, requests{}, bytes{};
  bool overflow{};
};
constinit Counters counters;
struct Guard {
  Guard() noexcept { while(counters.lock.test_and_set(std::memory_order_acquire)) {} }
  ~Guard() { counters.lock.clear(std::memory_order_release); }
};
void record(std::size_t bytes) noexcept {
  // The default/disabled hot path has one relaxed load and no lock or allocation.
  if(!counters.enabled.load(std::memory_order_relaxed)) return;
  Guard guard;
  if(!counters.scopes) return;
  constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
  if(counters.requests==maximum) counters.overflow=true;
  else ++counters.requests;
  static_assert(sizeof(std::size_t)<=sizeof(std::uint64_t));
  if(bytes>maximum-counters.bytes) { counters.bytes=maximum; counters.overflow=true; }
  else counters.bytes+=static_cast<std::uint64_t>(bytes);
}
#if defined(RACE_DESKTOP_PROFILE_TESTING) && RACE_DESKTOP_PROFILE_TESTING
thread_local std::size_t failures{};
thread_local std::size_t failure_after{std::numeric_limits<std::size_t>::max()};
#endif
void* attempt(std::size_t bytes, std::size_t alignment, bool aligned) noexcept {
#if defined(RACE_DESKTOP_PROFILE_TESTING) && RACE_DESKTOP_PROFILE_TESTING
  if(failures) { --failures; return nullptr; }
  if(failure_after!=std::numeric_limits<std::size_t>::max()) {
    if(!failure_after) { failure_after=std::numeric_limits<std::size_t>::max(); return nullptr; }
    --failure_after;
  }
#endif
  // Reject impossible object sizes without overflowing rounding arithmetic or
  // invoking the Microsoft CRT invalid-parameter handler. No private headers.
  constexpr auto maximum=static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
  if(bytes>maximum) return nullptr;
  const auto actual=bytes ? bytes : std::size_t{1};
  if(!aligned) return std::malloc(actual);
  if(!alignment || (alignment&(alignment-1))!=0) return nullptr;
  if(alignment<sizeof(void*)) alignment=sizeof(void*);
  if(alignment>maximum-sizeof(void*) || actual>maximum-alignment-sizeof(void*)) return nullptr;
#ifdef _WIN32
  return _aligned_malloc(actual,alignment);
#else
  void* result=nullptr;
  return posix_memalign(&result,alignment,actual)==0 ? result : nullptr;
#endif
}
void* allocate(std::size_t bytes, std::size_t alignment=0, bool aligned=false) {
  record(bytes); // One request, even when the handler retries the allocation.
  for(;;) {
    if(auto* pointer=attempt(bytes,alignment,aligned)) return pointer;
    const auto handler=std::get_new_handler();
    if(!handler) throw std::bad_alloc{};
    handler();
  }
}
void aligned_free(void* pointer) noexcept {
#ifdef _WIN32
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}
} // namespace

namespace race::desktop::profile {
bool allocation_counting_available() noexcept { return true; }
AllocationSnapshot allocation_snapshot() noexcept {
  Guard guard; return {counters.requests,counters.bytes,counters.overflow};
}
bool reset_allocation_counters() noexcept {
  Guard guard;
  if(counters.scopes) return false;
  counters.requests=0; counters.bytes=0; counters.overflow=false; return true;
}
AllocationScope::AllocationScope(bool enable) noexcept {
  if(!enable) return;
  Guard guard;
  if(counters.scopes==std::numeric_limits<std::uint64_t>::max()) { counters.overflow=true; return; }
  ++counters.scopes; active_=true; counters.enabled.store(true,std::memory_order_relaxed);
}
AllocationScope::~AllocationScope() { stop(); }
void AllocationScope::stop() noexcept {
  if(!active_) return;
  Guard guard;
  active_=false;
  if(--counters.scopes==0) counters.enabled.store(false,std::memory_order_relaxed);
}
#if defined(RACE_DESKTOP_PROFILE_TESTING) && RACE_DESKTOP_PROFILE_TESTING
namespace testing {
void fail_next_allocation_attempts(std::size_t count) noexcept { failures=count; failure_after=std::numeric_limits<std::size_t>::max(); }
void fail_allocation_after(std::size_t count) noexcept { failures=0; failure_after=count; }
bool seed_counters(std::uint64_t requests, std::uint64_t bytes) noexcept {
  Guard guard;
  if(counters.scopes) return false;
  counters.requests=requests; counters.bytes=bytes; counters.overflow=false; return true;
}
}
#endif
} // namespace race::desktop::profile

// Keep the CRT's malloc/free and aligned-malloc/aligned-free pairing. Do not add
// ownership headers or redirect DLL allocation APIs. In particular, these
// definitions provide no visibility into DLL-private operator new implementations.
#ifdef _MSC_VER
// Match the CRT allocation contracts, including overloads whose prior
// declarations are not exposed to MSVC's _Use_decl_annotations_ lookup.
#define RACE_NEW_CONTRACT(n) _Ret_notnull_ _Post_writable_byte_size_(n)
#define RACE_NOTHROW_NEW_CONTRACT(n) _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(n)
#else
#define RACE_NEW_CONTRACT(n)
#define RACE_NOTHROW_NEW_CONTRACT(n)
#endif
RACE_NEW_CONTRACT(n) void* operator new(std::size_t n) { return allocate(n); }
RACE_NEW_CONTRACT(n) void* operator new[](std::size_t n) { return allocate(n); }
RACE_NOTHROW_NEW_CONTRACT(n) void* operator new(std::size_t n,const std::nothrow_t&) noexcept { try{return allocate(n);}catch(...){return nullptr;} }
RACE_NOTHROW_NEW_CONTRACT(n) void* operator new[](std::size_t n,const std::nothrow_t&) noexcept { try{return allocate(n);}catch(...){return nullptr;} }
RACE_NEW_CONTRACT(n) void* operator new(std::size_t n,std::align_val_t a) { return allocate(n,static_cast<std::size_t>(a),true); }
RACE_NEW_CONTRACT(n) void* operator new[](std::size_t n,std::align_val_t a) { return allocate(n,static_cast<std::size_t>(a),true); }
RACE_NOTHROW_NEW_CONTRACT(n) void* operator new(std::size_t n,std::align_val_t a,const std::nothrow_t&) noexcept { try{return allocate(n,static_cast<std::size_t>(a),true);}catch(...){return nullptr;} }
RACE_NOTHROW_NEW_CONTRACT(n) void* operator new[](std::size_t n,std::align_val_t a,const std::nothrow_t&) noexcept { try{return allocate(n,static_cast<std::size_t>(a),true);}catch(...){return nullptr;} }
#undef RACE_NEW_CONTRACT
#undef RACE_NOTHROW_NEW_CONTRACT
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void operator delete(void* p,const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p,const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p,std::align_val_t) noexcept { aligned_free(p); }
void operator delete[](void* p,std::align_val_t) noexcept { aligned_free(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { aligned_free(p); }
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { aligned_free(p); }
void operator delete(void* p,std::align_val_t,const std::nothrow_t&) noexcept { aligned_free(p); }
void operator delete[](void* p,std::align_val_t,const std::nothrow_t&) noexcept { aligned_free(p); }

#else
namespace race::desktop::profile {
bool allocation_counting_available() noexcept { return false; }
AllocationSnapshot allocation_snapshot() noexcept { return {}; }
bool reset_allocation_counters() noexcept { return true; }
AllocationScope::AllocationScope(bool) noexcept {}
AllocationScope::~AllocationScope()=default;
void AllocationScope::stop() noexcept {}
}
#endif
