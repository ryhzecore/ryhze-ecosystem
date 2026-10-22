#pragma once
#include "startup_measurement.hpp"
#include <QSettings>
#include <QWidget>
#include <optional>

namespace race::desktop::startup {
inline std::optional<EntryStages> capture_entry_stages(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (argv[i] && std::string_view(argv[i]) == "--startup-measurement") return EntryStages{};
  return std::nullopt;
}
inline void size_editor_viewport(Witness* witness, QWidget* container) {
  if (!witness) { container->setMinimumSize(280,220); return; }
  // The admitted measurement host uses 96 DPI; reject a different pixel scale.
  if (container->devicePixelRatioF() != 1.0)
    throw std::runtime_error("startup measurement requires a 1:1 viewport pixel scale");
  container->setFixedSize(1280,720);
}
inline void isolate_editor_settings(const QString& directory) {
  // Only measurement opts in. Never open or alter the user's native settings.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory + "/settings");
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory + "/system-settings");
}
template<class Viewport> void editor_frame(Witness* witness, Viewport* viewport,
                                         HWND window, std::size_t objects, bool rendered) {
  if (!witness || !rendered) return;
  witness->visible(window);
  const auto& stats = viewport->renderer()->stats();
  if (!stats.has_image || stats.minimized || stats.diagnostics_incomplete ||
      stats.recreation_attempts || stats.removal_recovery_attempts)
    throw std::runtime_error("startup viewport image/lifecycle diagnostics invalid");
  witness->frame(objects, stats.width, stats.height, stats.device_epoch, stats.hardware,
                  stats.debug_errors, stats.debug_warnings, stats.adapter);
}
} // namespace race::desktop::startup
