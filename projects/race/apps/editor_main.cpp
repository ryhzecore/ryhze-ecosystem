// Editor process entry: argument validation, opt-in test/profile modes and startup.
#include "editor_window.hpp"

using namespace race_editor;

namespace {
// Command-line options with their defaults. main constructs it after the style is applied, which is when the
// shaders default reads the application folder.
struct Options {
  QString measurement;
  QString shaders = QCoreApplication::applicationDirPath() + "/shaders", scene, smoke, profile, file_mode,
          assets_path, mesh_product, material_product, package_path, checkpoint_project, checkpoint_file;
  bool file_profile = false, mesh_test = false, checkpoint_smoke = false, ui_smoke = false,
       authoring_smoke = false, authoring_uv_smoke = false, content_smoke = false, epoch_smoke = false,
       light_smoke = false;
  std::uint64_t profile_frames = 600;

  // Startup parts, called by main in this order. A nonzero result is main's exit code.
  int parse(const QStringList &args);
  int parse_path(const QString &option, const QString &value);
  int validate() const;
  int start_measurement(std::optional<desktop::startup::EntryStages> &startup_entry,
                        std::unique_ptr<desktop::startup::Witness> &startup) const;
  int run_editor(QApplication &app, Clock::time_point process_start,
                 desktop::startup::Witness *startup) const;
};

// Reads the option/value pairs in order; 64 rejects a missing value or an unknown or invalid option.
int Options::parse(const QStringList &args) {
  for (qsizetype i = 1; i < args.size(); ++i) {
    if (i + 1 >= args.size())
      return 64;
    auto option = args[i], raw = args[++i];
    if (option == "--startup-measurement") {
      measurement = QFileInfo(raw).absoluteFilePath();
      continue;
    }
    if (option == "--content-smoke") {
      smoke = QFileInfo(raw).absoluteFilePath();
      mesh_test = true;
      content_smoke = true;
      continue;
    }
    if (option == "--material-product") {
      material_product = raw;
      continue;
    }
    if (option == "--mesh-product") {
      mesh_product = raw;
      continue;
    }
    if (option == "--file-mode") {
      if (raw != "sync" && raw != "async")
        return 64;
      file_mode = raw;
      continue;
    }
    if (option == "--frames") {
      bool valid = false;
      profile_frames = raw.toULongLong(&valid);
      if (!valid || profile_frames < 60 || profile_frames > 3600)
        return 64;
      continue;
    }
    auto value = QFileInfo(raw).absoluteFilePath();
    if (const int usage = parse_path(option, value))
      return usage;
  }
  return 0;
}
// Options whose value is a path, already made absolute; 64 rejects an unknown option.
int Options::parse_path(const QString &option, const QString &value) {
  if (option == "--scene")
    scene = value;
  else if (option == "--shaders")
    shaders = value;
  else if (option == "--assets")
    assets_path = value;
  else if (option == "--package")
    package_path = value;
  else if (option == "--checkpoint-project")
    checkpoint_project = value;
  else if (option == "--checkpoint-file")
    checkpoint_file = value;
  else if (option == "--authoring-uv-smoke") {
    smoke = value;
    mesh_test = true;
    checkpoint_smoke = true;
    authoring_smoke = true;
    authoring_uv_smoke = true;
  } else if (option == "--authoring-smoke") {
    smoke = value;
    mesh_test = true;
    checkpoint_smoke = true;
    authoring_smoke = true;
  } else if (option == "--checkpoint-smoke") {
    smoke = value;
    mesh_test = true;
    checkpoint_smoke = true;
  } else if (option == "--mesh-smoke") {
    smoke = value;
    mesh_test = true;
  } else if (option == "--epoch-smoke") {
    smoke = value;
    epoch_smoke = true;
  } else if (option == "--light-smoke") {
    smoke = value;
    light_smoke = true;
  } else if (option == "--ui-smoke") {
    smoke = value;
    ui_smoke = true;
  } else if (option == "--smoke")
    smoke = value;
  else if (option == "--profile")
    profile = value;
  else if (option == "--file-profile") {
    profile = value;
    file_profile = true;
  } else
    return 64;
  return 0;
}
// Rejects incompatible option combinations with 64, checked in this order.
int Options::validate() const {
  if (!material_product.isEmpty() &&
      (!mesh_test || mesh_product.isEmpty() || assets_path.isEmpty() || !package_path.isEmpty()))
    return 64;
  if (epoch_smoke &&
      (!scene.isEmpty() || !package_path.isEmpty() || !assets_path.isEmpty() || !profile.isEmpty() ||
       mesh_test || ui_smoke || !checkpoint_project.isEmpty() || !checkpoint_file.isEmpty()))
    return 64;
  if (ui_smoke &&
      (!scene.isEmpty() || !package_path.isEmpty() || !assets_path.isEmpty() || !profile.isEmpty() ||
       mesh_test || !checkpoint_project.isEmpty() || !checkpoint_file.isEmpty()))
    return 64;
  if (light_smoke &&
      (!scene.isEmpty() || !package_path.isEmpty() || !assets_path.isEmpty() || !profile.isEmpty() ||
       mesh_test || ui_smoke || epoch_smoke || !checkpoint_project.isEmpty() || !checkpoint_file.isEmpty()))
    return 64;
  if (checkpoint_smoke != !checkpoint_project.isEmpty())
    return 64;
  if (checkpoint_project.isEmpty() != checkpoint_file.isEmpty() ||
      (!checkpoint_project.isEmpty() && (!mesh_test || smoke.isEmpty() || assets_path.isEmpty() ||
                                         mesh_product.isEmpty() || !package_path.isEmpty())))
    return 64;
  if (file_profile != !file_mode.isEmpty() || (file_profile && profile_frames != 600))
    return 64;
  if ((!profile.isEmpty() && !smoke.isEmpty()) || (!mesh_product.isEmpty() && !mesh_test))
    return 64;
  return 0;
}
// Startup measurement needs a Scene and no other output mode (64 otherwise). It isolates the editor
// settings before creating the witness.
int Options::start_measurement(std::optional<desktop::startup::EntryStages> &startup_entry,
                               std::unique_ptr<desktop::startup::Witness> &startup) const {
  if (!measurement.isEmpty()) {
    if (scene.isEmpty() || !smoke.isEmpty() || !profile.isEmpty() || !package_path.isEmpty() ||
        !assets_path.isEmpty())
      return 64;
    desktop::startup::isolate_editor_settings(measurement);
    startup = std::make_unique<desktop::startup::Witness>(native_path(measurement), "editor",
                                                          startup_entry ? &*startup_entry : nullptr);
  }
  return 0;
}
// Constructs the editor, opens the requested package, assets folder and Scene, then shows the window and
// runs the event loop. The editor is destroyed here, before main's witness and allocation scope.
int Options::run_editor(QApplication &app, Clock::time_point process_start,
                        desktop::startup::Witness *startup) const {
  auto document = desktop::Document::create();
  auto services = platform::make_services();
  if (!document || !services) {
    QMessageBox::critical(nullptr, "RACE startup",
                          !document ? document.error().message : services.error().message);
    return 1;
  }
  Editor editor(std::move(document).value(), std::move(services).value(), shaders, smoke, profile,
                profile_frames, process_start, file_mode, mesh_test, mesh_product, checkpoint_project,
                checkpoint_file, ui_smoke, authoring_smoke, material_product, authoring_uv_smoke,
                content_smoke, epoch_smoke, light_smoke, startup);
  if (!package_path.isEmpty() && (!assets_path.isEmpty() || !scene.isEmpty() || !mesh_product.isEmpty()))
    return 64;
  if (!package_path.isEmpty() && !editor.open_package(package_path, true))
    return 1;
  if (!assets_path.isEmpty() && !editor.choose_assets(assets_path))
    return 1;
  if (!scene.isEmpty() && !editor.open(scene, true))
    return 1;
  editor.startup_adopted();
  editor.show();
  return app.exec();
}
} // namespace

int main(int argc, char **argv) {
  const auto process_start = Clock::now();
  auto startup_entry = desktop::startup::capture_entry_stages(argc, argv);
  QApplication app(argc, argv);
  app.setApplicationName("RACE ENGINE");
  app.setOrganizationName("RYHZE");
  app.setStyle("Fusion");
  if (startup_entry)
    startup_entry->qapplication_ready();
  try {
    desktop::ui::apply(app);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "RACE style: %s\n", e.what());
    return 1;
  }
  if (startup_entry)
    startup_entry->style_ready();
  std::unique_ptr<desktop::startup::Witness> startup;
  Options options;
  auto args = app.arguments();
  if (const int usage = options.parse(args))
    return usage;
  if (startup_entry)
    startup_entry->arguments_ready();
  if (const int usage = options.validate())
    return usage;
  if (!options.profile.isEmpty()) {
    if (QFileInfo::exists(options.profile) || !QDir().mkpath(options.profile))
      return 65;
    QTimer::singleShot(180000, &app, [] { QCoreApplication::exit(3); });
  }
  desktop::profile::AllocationScope allocation_scope(!options.profile.isEmpty());
  if (!options.smoke.isEmpty()) {
    if (QFileInfo::exists(options.smoke) || !QDir().mkpath(options.smoke))
      return 65;
    QTimer::singleShot(30000, &app, [] { QCoreApplication::exit(3); });
  }
  try {
    if (const int usage = options.start_measurement(startup_entry, startup))
      return usage;
    return options.run_editor(app, process_start, startup.get());
  } catch (const std::exception &e) {
    if (!options.smoke.isEmpty() || !options.profile.isEmpty() || !options.measurement.isEmpty())
      std::fprintf(stderr, "RACE startup: %s\n", e.what());
    else
      QMessageBox::critical(nullptr, "RACE startup", e.what());
    return 1;
  }
}
