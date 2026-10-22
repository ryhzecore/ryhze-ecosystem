cmake_minimum_required(VERSION 3.14)
project(runner LANGUAGES CXX)

# Define the application target. To change its name, change BINARY_NAME in the
# top-level CMakeLists.txt, not the value here, or `flutter run` will no longer
# work.
#
# Any new source files that you add to the application should be added here.
add_executable(${BINARY_NAME} WIN32
  "flutter_window.cpp"
  "host_lifecycle.cpp"
  "host_activation.cpp"
  "host_security.cpp"
  "host_updates.cpp"
  "host_taskbar.cpp"
  "host_notifications.cpp"
  "host_installations.cpp"
  "host_content_staging.cpp"
  "host_content_transfer.cpp"
  "content_transfer.cpp"
  "host_artwork_cache.cpp"
  "artwork_cache.cpp"
  "content_stager.cpp"
  "steam_inventory.cpp"
  "update_stager.cpp"
  "secure_files.cpp"
  "credential_vault.cpp"
  "main.cpp"
  "utils.cpp"
  "win32_window.cpp"
  "${FLUTTER_MANAGED_DIR}/generated_plugin_registrant.cc"
  "Runner.rc"
  "runner.exe.manifest"
)

# Apply the standard set of build settings. This can be removed for applications
# that need different build settings.
apply_standard_settings(${BINARY_NAME})
# C++/WinRT notifications require standard coroutines on this SDK/toolchain.
target_compile_features(${BINARY_NAME} PRIVATE cxx_std_20)

# Add preprocessor definitions for the build version.
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION=\"${FLUTTER_VERSION}\"")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MAJOR=${FLUTTER_VERSION_MAJOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_MINOR=${FLUTTER_VERSION_MINOR}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_PATCH=${FLUTTER_VERSION_PATCH}")
target_compile_definitions(${BINARY_NAME} PRIVATE "FLUTTER_VERSION_BUILD=${FLUTTER_VERSION_BUILD}")

# Disable Windows macros that collide with C++ standard library functions.
target_compile_definitions(${BINARY_NAME} PRIVATE "NOMINMAX")

# Add dependency libraries and include directories. Add any application-specific
# dependencies here.
target_link_libraries(${BINARY_NAME} PRIVATE flutter flutter_wrapper_app)
target_link_libraries(${BINARY_NAME} PRIVATE "dwmapi.lib")
target_link_libraries(${BINARY_NAME} PRIVATE crypt32 bcrypt advapi32 shell32 ole32)
target_link_libraries(${BINARY_NAME} PRIVATE wintrust)
target_link_libraries(${BINARY_NAME} PRIVATE uuid)
target_link_libraries(${BINARY_NAME} PRIVATE runtimeobject windowsapp propsys)
target_include_directories(${BINARY_NAME} PRIVATE "${CMAKE_SOURCE_DIR}")

# Run the Flutter tool portions of the build. This must not be removed.
add_dependencies(${BINARY_NAME} flutter_assemble)
