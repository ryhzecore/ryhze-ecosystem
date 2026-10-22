cmake_minimum_required(VERSION 3.28)
# A separate, non-shipping build: SDK setup cannot change RACE compiler flags.
project(RACEPhysicsQualification LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The first Jolt qualification profile requires Windows MSVC x64")
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
  message(FATAL_ERROR "Only Debug and Release are admitted qualification configurations")
endif()
find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter)
get_filename_component(RACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${RACE_ROOT}/scripts/physics_dependencies.py"
  RESULT_VARIABLE verified OUTPUT_VARIABLE dependency ERROR_VARIABLE dependency_error)
if(NOT verified EQUAL 0)
  message(FATAL_ERROR "Jolt identity check failed: ${dependency} ${dependency_error}")
endif()
string(JSON PHYSICS_REPO_ROOT GET "${dependency}" source_root)
file(TO_CMAKE_PATH "${PHYSICS_REPO_ROOT}" PHYSICS_REPO_ROOT)
file(WRITE "${CMAKE_BINARY_DIR}/dependency-identity.json" "${dependency}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# Match the approved SDK boundary without enabling compiler parallelism or /fp:fast.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
add_compile_options(/W4 /WX /wd4577 /GR- /fp:precise /fp:except- /utf-8 /Zc:__cplusplus)
add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0 NOMINMAX _CRT_SECURE_NO_WARNINGS)

foreach(option IN ITEMS JPH_BUILD_SHARED_LIBS INTERPROCEDURAL_OPTIMIZATION
    OVERRIDE_CXX_FLAGS ENABLE_INSTALL ENABLE_OBJECT_STREAM DISABLE_CUSTOM_ALLOCATOR
    CPP_EXCEPTIONS_ENABLED CPP_RTTI_ENABLED USE_STD_VECTOR
    USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
    USE_WASM_SIMD JPH_USE_WASM64 CROSS_COMPILE_ARM
    JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
    DEBUG_RENDERER_IN_DEBUG_AND_RELEASE DEBUG_RENDERER_IN_DISTRIBUTION
    PROFILER_IN_DEBUG_AND_RELEASE PROFILER_IN_DISTRIBUTION JPH_USE_EXTERNAL_PROFILE
    TRACK_BROADPHASE_STATS TRACK_NARROWPHASE_STATS JPH_TRACK_SIMULATION_STATS
    FLOATING_POINT_EXCEPTIONS_ENABLED USE_ASSERTS)
  set(${option} OFF)
endforeach()
set(DOUBLE_PRECISION ON)
set(CROSS_PLATFORM_DETERMINISTIC ON)
set(OBJECT_LAYER_BITS 16)
set(GENERATE_DEBUG_SYMBOLS ON)
# Only the library source list is consumed, never Build/CMakeLists.txt or examples.
include("${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake")
# Original dependency bytes stay pinned. Compile one explicitly identified
# derived TU to correct the reproduced fixed BuildTree stack overflow.
set(RACE_TREE_OVERLAY "${CMAKE_BINARY_DIR}/generated/race-jolt-tree-stack-256-v1/QuadTree.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${RACE_ROOT}/scripts/physics_build_overlay.py"
  --source "${PHYSICS_REPO_ROOT}" --output "${RACE_TREE_OVERLAY}"
  RESULT_VARIABLE overlay_status OUTPUT_VARIABLE overlay ERROR_VARIABLE overlay_error)
if(NOT overlay_status EQUAL 0)
  message(FATAL_ERROR "Jolt build overlay check failed: ${overlay_error}")
endif()
string(JSON dependency SET "${dependency}" build_overlay "${overlay}")
# Keep the second approved correction as a separately pinned header overlay.
set(RACE_EPA_INCLUDE "${CMAKE_BINARY_DIR}/generated/race-jolt-epa-capacity-256-v2/include")
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${RACE_ROOT}/scripts/physics_epa_overlay.py"
  --source "${PHYSICS_REPO_ROOT}" --output "${RACE_EPA_INCLUDE}/Jolt/Geometry/EPAConvexHullBuilder.h"
  RESULT_VARIABLE epa_status OUTPUT_VARIABLE epa_overlay ERROR_VARIABLE epa_error)
if(NOT epa_status EQUAL 0)
  message(FATAL_ERROR "Jolt EPA overlay check failed: ${epa_error}")
endif()
string(JSON dependency SET "${dependency}" epa_build_overlay "${epa_overlay}")
target_include_directories(Jolt BEFORE PUBLIC "${RACE_EPA_INCLUDE}")
file(WRITE "${CMAKE_BINARY_DIR}/dependency-identity.json" "${dependency}")
get_target_property(jolt_sources Jolt SOURCES)
set(original_tree "${PHYSICS_REPO_ROOT}/Jolt/Physics/Collision/BroadPhase/QuadTree.cpp")
list(FIND jolt_sources "${original_tree}" tree_index)
if(tree_index LESS 0)
  message(FATAL_ERROR "Expected exactly one upstream QuadTree translation unit")
endif()
list(REMOVE_AT jolt_sources ${tree_index})
list(FIND jolt_sources "${original_tree}" duplicate_tree_index)
if(NOT duplicate_tree_index LESS 0)
  message(FATAL_ERROR "Duplicate upstream QuadTree translation unit")
endif()
list(APPEND jolt_sources "${RACE_TREE_OVERLAY}")
set_property(TARGET Jolt PROPERTY SOURCES "${jolt_sources}")
set_target_properties(Jolt PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(race_jolt_abi abi.cpp)
set_target_properties(race_jolt_abi PROPERTIES CXX_STANDARD 20)
target_link_libraries(race_jolt_abi PRIVATE Jolt)
add_executable(race_jolt_allocation_probe allocation_probe.cpp owned_leases_probe.cpp product_owner_probe.cpp
  "${RACE_ROOT}/src/assets/collision_wire.cpp" "${RACE_ROOT}/src/assets/digest.cpp"
  "${RACE_ROOT}/src/foundation/digest.cpp" "${RACE_ROOT}/src/physics/profile.cpp")
# Only this non-SDK owner TU contains prepublication allocation failures. The
# SDK and allocation_probe retain their existing exception-free compilation.
set_source_files_properties(owned_leases_probe.cpp product_owner_probe.cpp
  "${RACE_ROOT}/src/assets/collision_wire.cpp" "${RACE_ROOT}/src/assets/digest.cpp"
  PROPERTIES COMPILE_OPTIONS /EHsc)
set_target_properties(race_jolt_allocation_probe PROPERTIES CXX_STANDARD 20)
target_include_directories(race_jolt_allocation_probe PRIVATE "${RACE_ROOT}/include")
target_link_libraries(race_jolt_allocation_probe PRIVATE Jolt)
target_link_options(race_jolt_allocation_probe PRIVATE /STACK:1048576)
enable_testing()
add_test(NAME jolt_abi COMMAND race_jolt_abi)
set_tests_properties(jolt_abi PROPERTIES TIMEOUT 30)
add_executable(race_physics_profile_tests ../physics_profile_tests.cpp "${RACE_ROOT}/src/physics/profile.cpp")
target_include_directories(race_physics_profile_tests PRIVATE "${RACE_ROOT}/include")
set_target_properties(race_physics_profile_tests PROPERTIES CXX_STANDARD 20)
add_test(NAME physics_primitive_profile COMMAND race_physics_profile_tests)
set_tests_properties(physics_primitive_profile PROPERTIES TIMEOUT 30)
