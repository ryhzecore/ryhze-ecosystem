#include "mesh_render.hpp"

namespace race::desktop {
Result<std::shared_ptr<const render::ViewportGeometry>> copy_mesh_geometry(
    const assets::mesh::Lease &lease) {
  if (!lease.operator->()) return fail(ErrorCode::InvalidInput, "viewport bridge requires an owning lease");
  const auto &view = lease->view();
  if (view.stamp().representation != assets::mesh::Representation::Cooked)
    return fail(ErrorCode::InvalidInput, "viewport bridge requires cooked mesh input");
  if (view.vertex_count() > render::viewport_mesh_max_vertices ||
      view.index_count() > render::viewport_mesh_max_indices)
    return fail(ErrorCode::Exhausted, "viewport mesh bridge capacity exceeded");
  try {
    std::vector<render::GeometryVertex> vertices; vertices.reserve(view.vertex_count());
    std::vector<std::uint32_t> indices; indices.reserve(view.index_count());
    for (std::size_t i = 0; i < view.vertex_count(); ++i) {
      const auto v = view.vertex(i).value(); vertices.push_back({v.position, v.normal,v.uv});
    }
    for (std::size_t i = 0; i < view.index_count(); ++i) indices.push_back(view.index(i).value());
    return render::ViewportGeometry::create(vertices, indices,view.has_uv());
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "viewport mesh bridge allocation"); }
}
} // namespace race::desktop
