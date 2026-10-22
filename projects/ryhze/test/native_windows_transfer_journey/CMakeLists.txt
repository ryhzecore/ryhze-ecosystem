cmake_minimum_required(VERSION 3.14)
project(ryhze_transfer_journey LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This acceptance runner requires Windows and MSVC.")
endif()
get_filename_component(RYHZE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FLUTTER_EPHEMERAL "${RYHZE_ROOT}/windows/flutter/ephemeral" CACHE PATH
    "Existing pinned Flutter Windows engine and wrapper; no SDK download")
set(WRAPPER "${FLUTTER_EPHEMERAL}/cpp_client_wrapper")
if(NOT EXISTS "${FLUTTER_EPHEMERAL}/flutter_windows.dll.lib" OR
   NOT EXISTS "${WRAPPER}/flutter_view_controller.cc")
  message(FATAL_ERROR "Build the debug journey entrypoint with the pinned SDK first.")
endif()
# This standalone project is never included by the production CMake projects.
# No install/package target and no change to the production runner composition.
add_executable(ryhze_transfer_journey WIN32
  main.cpp
  "${RYHZE_ROOT}/windows/runner/win32_window.cpp"
  "${RYHZE_ROOT}/windows/runner/utils.cpp"
  "${RYHZE_ROOT}/windows/runner/host_content_transfer.cpp"
  "${RYHZE_ROOT}/windows/runner/host_content_staging.cpp"
  "${RYHZE_ROOT}/windows/runner/content_transfer.cpp"
  "${RYHZE_ROOT}/windows/runner/content_stager.cpp"
  "${RYHZE_ROOT}/windows/runner/secure_files.cpp"
  "${RYHZE_ROOT}/windows/runner/runner.exe.manifest"
  "${WRAPPER}/core_implementations.cc"
  "${WRAPPER}/standard_codec.cc"
  "${WRAPPER}/flutter_engine.cc"
  "${WRAPPER}/flutter_view_controller.cc"
)
target_compile_features(ryhze_transfer_journey PRIVATE cxx_std_17)
target_compile_definitions(ryhze_transfer_journey PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(ryhze_transfer_journey PRIVATE /W4 /WX /EHsc /wd4100)
target_include_directories(ryhze_transfer_journey PRIVATE
  "${RYHZE_ROOT}/windows/runner" "${WRAPPER}/include" "${FLUTTER_EPHEMERAL}")
target_link_libraries(ryhze_transfer_journey PRIVATE
  "${FLUTTER_EPHEMERAL}/flutter_windows.dll.lib" bcrypt shell32 ole32 dwmapi advapi32)
