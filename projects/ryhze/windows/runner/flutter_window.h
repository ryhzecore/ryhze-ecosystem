#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>

#include <memory>

#include "host_lifecycle.h"
#include "host_activation.h"
#include "host_security.h"
#include "host_updates.h"
#include "host_taskbar.h"
#include "host_notifications.h"
#include "host_installations.h"
#include "host_content_staging.h"
#include "host_content_transfer.h"
#include "host_artwork_cache.h"
#include "win32_window.h"

// A window that does nothing but host a Flutter view.
class FlutterWindow : public Win32Window {
 public:
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  FlutterWindow(const flutter::DartProject& project, HostActivation& activation);
  virtual ~FlutterWindow();

 protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

 private:
  // The project to run.
  flutter::DartProject project_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  // Retain the last power/session state even before Dart connects.
  HostLifecycle host_lifecycle_;
  HostActivation& host_activation_;
  HostSecurity host_security_;
  HostUpdates host_updates_;
  HostTaskbar host_taskbar_;
  HostNotifications host_notifications_;
  HostInstallations host_installations_;
  HostContentStaging host_content_staging_;
  HostContentTransfer host_content_transfer_;
  HostArtworkCache host_artwork_cache_;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
