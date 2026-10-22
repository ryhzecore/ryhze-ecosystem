// Isolated SDK ABI evidence. No Physics service, hooks, factory or world is created.
#include <Jolt/Jolt.h>
#include <Jolt/ConfigurationString.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <Jolt/Physics/LargeIslandSplitter.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <cstdio>

#if !defined(JPH_DOUBLE_PRECISION) || !defined(JPH_CROSS_PLATFORM_DETERMINISTIC)
#error Qualification requires the approved precision and arithmetic profile
#endif
#if defined(JPH_USE_SSE4_1) || defined(JPH_USE_SSE4_2) || defined(JPH_USE_AVX) || defined(JPH_USE_AVX2) || defined(JPH_USE_AVX512) || defined(JPH_USE_FMADD) || defined(JPH_USE_F16C) || defined(JPH_USE_LZCNT) || defined(JPH_USE_TZCNT)
#error Qualification requires the SSE2 baseline
#endif
#if defined(JPH_DISABLE_CUSTOM_ALLOCATOR) || defined(JPH_DISABLE_TEMP_ALLOCATOR) || defined(JPH_PROFILE_ENABLED) || defined(JPH_EXTERNAL_PROFILE) || defined(JPH_DEBUG_RENDERER) || defined(JPH_OBJECT_STREAM) || defined(JPH_TRACK_SIMULATION_STATS) || defined(JPH_USE_DX12) || defined(JPH_USE_VK) || defined(JPH_USE_MTL) || defined(JPH_USE_CPU_COMPUTE)
#error Unadmitted allocator or instrumentation configuration
#endif
static_assert(JPH_VERSION_MAJOR == 5 && JPH_VERSION_MINOR == 6 && JPH_VERSION_PATCH == 0);
static_assert(sizeof(void*) == 8 && sizeof(JPH::ObjectLayer) == 2);
static_assert(JPH_CACHE_LINE_SIZE == 64 && _ITERATOR_DEBUG_LEVEL == 0);
// Legal protected access, without rewriting SDK access specifiers or sources.
struct JobLayout : JPH::JobSystem {
  static constexpr std::size_t job_size = sizeof(Job);
  static constexpr std::size_t slot_size = JPH::FixedSizeFreeList<Job>::ObjectStorageSize;
};
template<class T> void layout(const char* name) {
  std::printf("\"%s\":{\"bytes\":%zu,\"alignment\":%zu},\n",name,sizeof(T),alignof(T));
}
int main() {
  const bool compatible=JPH::VerifyJoltVersionID();
  std::printf("{\"schema\":1,\"abi_compatible\":%s,\"compiler_msc_full_ver\":%d,\n",
              compatible?"true":"false",_MSC_FULL_VER);
  std::printf("\"configuration\":\"%s\",\n",JPH::GetConfigurationString());
  layout<JPH::PhysicsUpdateContext::Step>("step");
  layout<JPH::PhysicsUpdateContext::JobHandleArray>("job_handle_array");
  layout<JPH::JobSystem::JobFunction>("job_function");
  layout<JPH::JobSystem::JobHandle>("job_handle");
  layout<JPH::Body>("body");
  layout<JPH::MotionProperties>("motion_properties");
  layout<JPH::BoxShape>("box_shape");
  layout<JPH::SphereShape>("sphere_shape");
  layout<JPH::LargeIslandSplitter::Splits>("island_splits");
  layout<JPH::PhysicsSystem>("physics_system");
  std::printf("\"job_bytes\":%zu,\"job_slot_bytes\":%zu,\n",JobLayout::job_size,JobLayout::slot_size);
  std::printf("\"allocation_proved\":false,\"simulation_executed\":false}\n");
  return compatible?0:1;
}
