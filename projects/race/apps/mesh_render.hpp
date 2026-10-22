#pragma once
#include "race/assets/mesh.hpp"
#include "race/render/viewport_geometry.hpp"

namespace race::desktop {
// Executable composition only: the lease stays alive throughout the copy, and
// the returned packet owns all geometry without a dependency on the asset store.
Result<std::shared_ptr<const render::ViewportGeometry>> copy_mesh_geometry(
    const assets::mesh::Lease &);
} // namespace race::desktop
