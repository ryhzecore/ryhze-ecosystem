cmake_minimum_required(VERSION 3.14)
project(ryhze_native_lifecycle_tests LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This harness requires Windows and MSVC.")
endif()
get_filename_component(RYHZE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(FLUTTER_EPHEMERAL "${RYHZE_ROOT}/windows/flutter/ephemeral" CACHE PATH
    "Existing generated Flutter Windows client wrapper; no SDK download")
set(WRAPPER "${FLUTTER_EPHEMERAL}/cpp_client_wrapper")
if(NOT EXISTS "${WRAPPER}/standard_codec.cc")
  message(FATAL_ERROR "Prepare the pinned Windows Flutter SDK before this test.")
endif()
add_executable(host_lifecycle_test
  host_lifecycle_test.cpp
  engine_api_forbidden.cpp
  "${RYHZE_ROOT}/windows/runner/host_lifecycle.cpp"
  "${WRAPPER}/standard_codec.cc"
  "${WRAPPER}/core_implementations.cc"
)
target_compile_features(host_lifecycle_test PRIVATE cxx_std_17)
target_compile_definitions(host_lifecycle_test PRIVATE NOMINMAX FLUTTER_DESKTOP_LIBRARY)
target_include_directories(host_lifecycle_test PRIVATE
  "${RYHZE_ROOT}/windows/runner" "${WRAPPER}/include" "${FLUTTER_EPHEMERAL}")
# The fake messenger and real StandardMethodCodec exercise the bridge without
# loading Flutter. Engine API stubs abort if any engine-backed path is reached.
target_compile_options(host_lifecycle_test PRIVATE /W4 /WX /EHsc /Gy)
target_link_options(host_lifecycle_test PRIVATE /OPT:REF /OPT:ICF)
enable_testing()
add_test(NAME host_lifecycle COMMAND host_lifecycle_test)

function(ryhze_native_test name)
  add_executable(${name}_test ${name}_test.cpp ${ARGN})
  target_compile_features(${name}_test PRIVATE cxx_std_17)
  target_compile_definitions(${name}_test PRIVATE NOMINMAX FLUTTER_DESKTOP_LIBRARY)
  target_include_directories(${name}_test PRIVATE "${RYHZE_ROOT}/windows/runner" "${WRAPPER}/include" "${FLUTTER_EPHEMERAL}")
  target_compile_options(${name}_test PRIVATE /W4 /WX /EHsc /Gy)
  target_link_options(${name}_test PRIVATE /OPT:REF /OPT:ICF)
  add_test(NAME ${name} COMMAND ${name}_test)
endfunction()
ryhze_native_test(secure_files "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(secure_files_test PRIVATE bcrypt)
target_compile_definitions(secure_files_test PRIVATE RYHZE_SECURE_FILES_NATIVE_TEST)
ryhze_native_test(credential_vault "${RYHZE_ROOT}/windows/runner/credential_vault.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(credential_vault_test PRIVATE crypt32 bcrypt advapi32)
ryhze_native_test(host_activation "${RYHZE_ROOT}/windows/runner/host_activation.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_activation_test PRIVATE bcrypt advapi32)
ryhze_native_test(update_stager "${RYHZE_ROOT}/windows/runner/update_stager.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(update_stager_test PRIVATE bcrypt crypt32 wintrust)
ryhze_native_test(host_security "${RYHZE_ROOT}/windows/runner/host_security.cpp" "${RYHZE_ROOT}/windows/runner/credential_vault.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_security_test PRIVATE crypt32 bcrypt advapi32 shell32 ole32)
ryhze_native_test(host_updates "${RYHZE_ROOT}/windows/runner/host_updates.cpp" "${RYHZE_ROOT}/windows/runner/update_stager.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_updates_test PRIVATE bcrypt crypt32 wintrust)
target_compile_definitions(host_updates_test PRIVATE RYHZE_UPDATES_NATIVE_TEST)
ryhze_native_test(host_taskbar "${RYHZE_ROOT}/windows/runner/host_taskbar.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_taskbar_test PRIVATE ole32 uuid)

ryhze_native_test(host_notifications "${RYHZE_ROOT}/windows/runner/host_notifications.cpp" "${RYHZE_ROOT}/windows/runner/host_activation.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_notifications_test PRIVATE bcrypt advapi32 shell32 ole32 uuid runtimeobject windowsapp propsys)

ryhze_native_test(steam_inventory "${RYHZE_ROOT}/windows/runner/steam_inventory.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(steam_inventory_test PRIVATE advapi32 bcrypt)
ryhze_native_test(host_installations "${RYHZE_ROOT}/windows/runner/host_installations.cpp" "${RYHZE_ROOT}/windows/runner/steam_inventory.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_installations_test PRIVATE advapi32 bcrypt)
target_compile_features(host_notifications_test PRIVATE cxx_std_20)

ryhze_native_test(host_content_staging "${RYHZE_ROOT}/windows/runner/host_content_staging.cpp" "${RYHZE_ROOT}/windows/runner/content_stager.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_content_staging_test PRIVATE bcrypt shell32 ole32 user32)

ryhze_native_test(content_stager "${RYHZE_ROOT}/windows/runner/content_stager.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(content_stager_test PRIVATE bcrypt psapi)

ryhze_native_test(artwork_cache "${RYHZE_ROOT}/windows/runner/artwork_cache.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(artwork_cache_test PRIVATE bcrypt shell32 ole32)
ryhze_native_test(host_artwork_cache "${RYHZE_ROOT}/windows/runner/host_artwork_cache.cpp" "${RYHZE_ROOT}/windows/runner/artwork_cache.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_artwork_cache_test PRIVATE bcrypt shell32 ole32 user32)
target_compile_definitions(host_artwork_cache_test PRIVATE RYHZE_ARTWORK_NATIVE_TEST)

ryhze_native_test(content_transfer "${RYHZE_ROOT}/windows/runner/content_transfer.cpp" "${RYHZE_ROOT}/windows/runner/content_stager.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp")
target_link_libraries(content_transfer_test PRIVATE bcrypt)

ryhze_native_test(host_content_transfer "${RYHZE_ROOT}/windows/runner/host_content_transfer.cpp" "${RYHZE_ROOT}/windows/runner/content_transfer.cpp" "${RYHZE_ROOT}/windows/runner/secure_files.cpp" engine_api_forbidden.cpp "${WRAPPER}/standard_codec.cc" "${WRAPPER}/core_implementations.cc")
target_link_libraries(host_content_transfer_test PRIVATE bcrypt shell32 ole32 user32)
target_compile_definitions(host_content_transfer_test PRIVATE RYHZE_TRANSFER_NATIVE_TEST)
