#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"
#include "host_activation.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  const auto arguments = GetCommandLineArguments();
  if (arguments.size() > 1) return EXIT_FAILURE;
  HostActivation activation;
  const auto start = activation.Start(arguments.empty() ? std::nullopt :
      std::optional<std::string>(arguments.front()));
  if (start == HostActivation::StartResult::forwarded) return EXIT_SUCCESS;
  if (start != HostActivation::StartResult::primary) return EXIT_FAILURE;

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return EXIT_FAILURE;

  int exit_code = EXIT_SUCCESS;
  {
    flutter::DartProject project(L"data");

    // Activation is delivered through the bounded native channel, never Dart CLI.
    FlutterWindow window(project, activation);
    Win32Window::Point origin(10, 10);
    Win32Window::Size size(1280, 720);
    if (!window.Create(L"Ryhze", origin, size)) {
      exit_code = EXIT_FAILURE;
    } else {
      window.SetQuitOnClose(true);

      ::MSG msg;
      BOOL message_result;
      while ((message_result = ::GetMessage(&msg, nullptr, 0, 0)) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
      }
      if (message_result < 0) exit_code = EXIT_FAILURE;
    }
    // Destroy Flutter and its COM-backed native bridges while this apartment lives.
  }

  ::CoUninitialize();
  return exit_code;
}
