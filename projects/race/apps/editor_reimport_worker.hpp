#pragma once
#include "desktop_mesh_reimport.hpp"

namespace race::desktop {
// Windows executable composition only. The caller fixes this trusted sibling
// importer path; no project, recipe or request can choose an executable.
// Invocations run on the owning background worker. Each creates and retains a
// new pinned stage; failed attempts are preserved, never reused or removed.
MeshReimportTask::WorkerFactory editor_reimport_workers(std::string executable);
} // namespace race::desktop
