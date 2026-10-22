#include "windows_viewport_internal.hpp"

namespace race::render {
namespace windows_viewport_detail {
bool finite(ViewportVec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

ViewportVec3 subtract(ViewportVec3 a, ViewportVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

ViewportVec3 cross(ViewportVec3 a, ViewportVec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Result<ViewportVec3> normalized(ViewportVec3 p) {
  const double length = std::hypot(p.x, p.y, p.z);
  if (!finite(p) || !std::isfinite(length) || length < 1e-10)
    return fail(ErrorCode::InvalidInput, "viewport direction is degenerate");
  return ViewportVec3{p.x / length, p.y / length, p.z / length};
}

Matrix multiply(const Matrix &a, const Matrix &b) {
  Matrix out{};
  for (std::size_t r = 0; r < 4; ++r)
    for (std::size_t c = 0; c < 4; ++c)
      for (std::size_t k = 0; k < 4; ++k)
        out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
  return out;
}

Result<Matrix> view_projection(const ViewportCamera &camera, double aspect) {
  if (!finite(camera.eye) || !finite(camera.target) || !finite(camera.up) ||
      std::max({std::abs(camera.eye.x), std::abs(camera.eye.y), std::abs(camera.eye.z),
                std::abs(camera.target.x), std::abs(camera.target.y), std::abs(camera.target.z)}) > 1e12 ||
      !std::isfinite(camera.vertical_fov_radians) || camera.vertical_fov_radians < .05 ||
      camera.vertical_fov_radians > 3 || !std::isfinite(camera.near_metres) ||
      !std::isfinite(camera.far_metres) || camera.near_metres < .0001 ||
      camera.far_metres <= camera.near_metres || camera.far_metres > 1e7)
    return fail(ErrorCode::InvalidInput, "invalid bounded viewport camera");
  auto forward = normalized(subtract(camera.target, camera.eye));
  if (!forward)
    return forward.error();
  auto right = normalized(cross(forward.value(), camera.up));
  if (!right)
    return right.error();
  const auto up = cross(right.value(), forward.value());
  const auto r = right.value(), f = forward.value();
  const Matrix view{r.x, r.y, r.z, 0, up.x, up.y, up.z, 0, -f.x, -f.y, -f.z, 0, 0, 0, 0, 1};
  const double y = 1 / std::tan(camera.vertical_fov_radians * .5), n = camera.near_metres,
               z = camera.far_metres;
  const Matrix projection{y / aspect, 0, 0, 0, 0, y, 0, 0, 0, 0, z / (n - z), n * z / (n - z), 0, 0, -1, 0};
  return multiply(projection, view);
}

bool valid_color(const std::array<float, 4> &color) {
  return std::all_of(color.begin(), color.end(),
                     [](float v) { return std::isfinite(v) && v >= 0 && v <= 64; }) &&
         color[3] == 1;
}

Result<Constants> constants(const std::array<double, 12> &affine, const Matrix &vp, const RenderFrame &frame,
                            const std::array<float, 4> &color, ViewportVec3 light, bool unlit,
                            float expansion, bool camera_relative_geometry) {
  if (!valid_color(color))
    return fail(ErrorCode::InvalidInput, "invalid opaque linear viewport color");
  for (std::size_t i = 0; i < affine.size(); ++i)
    if (!std::isfinite(affine[i]) || std::abs(affine[i]) > (i % 4 == 3 ? 1e12 : 1e6))
      return fail(ErrorCode::InvalidInput, "viewport affine exceeds finite bounds");
  Matrix world = identity;
  std::copy(affine.begin(), affine.end(), world.begin());
  world[3] -= frame.camera.eye.x;
  world[7] -= frame.camera.eye.y;
  world[11] -= frame.camera.eye.z;
  if (!camera_relative_geometry &&
      std::max({std::abs(world[3]), std::abs(world[7]), std::abs(world[11])}) > 1e7)
    return fail(ErrorCode::Exhausted, "viewport camera-relative range exceeded");
  const auto &a = affine;
  const double determinant = a[0] * (a[5] * a[10] - a[6] * a[9]) - a[1] * (a[4] * a[10] - a[6] * a[8]) +
                             a[2] * (a[4] * a[9] - a[5] * a[8]);
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
    return fail(ErrorCode::InvalidInput, "singular viewport transform");
  const Matrix normal{(a[5] * a[10] - a[6] * a[9]) / determinant,
                      (a[6] * a[8] - a[4] * a[10]) / determinant,
                      (a[4] * a[9] - a[5] * a[8]) / determinant,
                      0,
                      (a[2] * a[9] - a[1] * a[10]) / determinant,
                      (a[0] * a[10] - a[2] * a[8]) / determinant,
                      (a[1] * a[8] - a[0] * a[9]) / determinant,
                      0,
                      (a[1] * a[6] - a[2] * a[5]) / determinant,
                      (a[2] * a[4] - a[0] * a[6]) / determinant,
                      (a[0] * a[5] - a[1] * a[4]) / determinant,
                      0,
                      0,
                      0,
                      0,
                      1};
  const auto mvp = camera_relative_geometry ? vp : multiply(vp, world);
  Constants result;
  for (std::size_t i = 0; i < 16; ++i) {
    if (!std::isfinite(mvp[i]) || std::abs(mvp[i]) > 1e30 || !std::isfinite(normal[i]) ||
        std::abs(normal[i]) > 1e30)
      return fail(ErrorCode::InvalidInput, "viewport float conversion exceeded");
    result.mvp[i] = static_cast<float>(mvp[i]);
    result.normal[i] = static_cast<float>(normal[i]);
  }
  result.color = color;
  result.light = {static_cast<float>(light.x), static_cast<float>(light.y), static_cast<float>(light.z),
                  frame.directional_intensity};
  result.parameters = {frame.ambient_intensity, unlit ? 1.f : 0.f, expansion, 0};
  return result;
}

Constants overlay_constants(const OverlayDraw &draw) noexcept {
  Constants result;
  for (std::size_t i = 0; i < identity.size(); ++i)
    result.mvp[i] = result.normal[i] = static_cast<float>(identity[i]);
  result.color = draw.color;
  result.light = {0, 0, 1, 0};
  result.parameters = {1, 1, 1, 0};
  return result;
}

std::vector<Vertex> geometry() {
  std::vector<Vertex> v;
  v.reserve(232);
  const auto face = [&](std::array<ViewportVec3, 4> p, ViewportVec3 n) {
    constexpr std::array<unsigned, 6> order{0, 1, 2, 0, 2, 3};
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    for (auto i : order)
      v.push_back({static_cast<float>(p[i].x), static_cast<float>(p[i].y), static_cast<float>(p[i].z),
                   static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z), uv[i][0],
                   uv[i][1]});
  };
  face({{{-.5, -.5, .5}, {.5, -.5, .5}, {.5, .5, .5}, {-.5, .5, .5}}}, {0, 0, 1});
  face({{{.5, -.5, -.5}, {-.5, -.5, -.5}, {-.5, .5, -.5}, {.5, .5, -.5}}}, {0, 0, -1});
  face({{{.5, -.5, .5}, {.5, -.5, -.5}, {.5, .5, -.5}, {.5, .5, .5}}}, {1, 0, 0});
  face({{{-.5, -.5, -.5}, {-.5, -.5, .5}, {-.5, .5, .5}, {-.5, .5, -.5}}}, {-1, 0, 0});
  face({{{-.5, .5, .5}, {.5, .5, .5}, {.5, .5, -.5}, {-.5, .5, -.5}}}, {0, 1, 0});
  face({{{-.5, -.5, -.5}, {.5, -.5, -.5}, {.5, -.5, .5}, {-.5, -.5, .5}}}, {0, -1, 0});
  const auto line = [&](ViewportVec3 a, ViewportVec3 b) {
    for (auto p : {a, b})
      v.push_back({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), 0, 1, 0});
  };
  for (int a : {-1, 1})
    for (int b : {-1, 1}) {
      line({-.5, a * .5, b * .5}, {.5, a * .5, b * .5});
      line({a * .5, -.5, b * .5}, {a * .5, .5, b * .5});
      line({a * .5, b * .5, -.5}, {a * .5, b * .5, .5});
    }
  for (int i = -20; i <= 20; ++i) {
    line({static_cast<double>(i), 0, -20}, {static_cast<double>(i), 0, 20});
    line({-20, 0, static_cast<double>(i)}, {20, 0, static_cast<double>(i)});
  }
  line({-20, .002, 0}, {20, .002, 0});
  line({0, .002, -20}, {0, .002, 20});
  return v;
}
} // namespace windows_viewport_detail

Result<void> WindowsViewport::Impl::validate_overlays(const RenderFrame &frame,
                                                      std::size_t &overlay_vertices) {
  for (const auto &overlay : frame.overlays) {
    if (overlay.vertices.size() > viewport_max_overlay_vertices - overlay_vertices)
      return fail(ErrorCode::Exhausted, "viewport aggregate overlay vertex capacity is 4096");
    if (overlay.vertices.empty() || overlay.vertices.size() % 3 != 0 || !valid_color(overlay.color))
      return fail(ErrorCode::InvalidInput, "viewport overlay requires complete opaque triangles");
    for (const auto &vertex : overlay.vertices)
      for (const float coordinate : vertex)
        if (!std::isfinite(coordinate) || coordinate < -16.f || coordinate > 16.f)
          return fail(ErrorCode::InvalidInput, "viewport overlay coordinate exceeds finite NDC bounds");
    overlay_vertices += overlay.vertices.size();
  }
  return {};
}

Result<void> WindowsViewport::Impl::collect_textures(const RenderFrame &frame, std::size_t &texture_count,
                                                     std::size_t &texture_bytes) {
  auto &p = *this;
  p.texture_packets.fill({});
  p.draw_textures.fill(0);
  for (std::size_t i = 0; i < frame.draws.size(); ++i) {
    const auto &draw = frame.draws[i];
    if (!draw.material || !draw.material->texture())
      continue;
    if (draw.geometry && !draw.geometry->has_uv())
      return fail(ErrorCode::InvalidInput, "textured viewport mesh requires explicit UV coordinates");
    const auto &packet = draw.material->texture();
    std::size_t unique{};
    while (unique < texture_count && p.texture_packets[unique] != packet)
      ++unique;
    if (unique == texture_count) {
      if (texture_count == viewport_max_textures ||
          packet->texels().size() > viewport_texture_frame_bytes - texture_bytes)
        return fail(ErrorCode::Exhausted, "viewport unique texture frame budget exceeded");
      p.texture_packets[texture_count++] = packet;
      texture_bytes += packet->texels().size();
    }
    p.draw_textures[i] = static_cast<UINT>(unique) + 1;
  }
  return {};
}

Result<void> WindowsViewport::Impl::reserve_mesh(const RenderFrame &frame, std::size_t overlay_vertices) {
  auto &p = *this;
  std::size_t mesh_vertices{}, mesh_indices{}, upload_vertices{};
  for (const auto &draw : frame.draws)
    if (draw.geometry) {
      const auto vc = draw.geometry->vertices().size(), ic = draw.geometry->indices().size();
      if (vc > viewport_mesh_frame_vertices - mesh_vertices ||
          ic > viewport_mesh_frame_indices - mesh_indices)
        return fail(ErrorCode::Exhausted, "viewport aggregate mesh frame capacity exceeded");
      mesh_vertices += vc;
      mesh_indices += ic;
      upload_vertices += ic * (draw.selected ? 3 : 1);
    }
  // Mesh/outline and overlay vertices share the same retained slot upload.
  // Both totals were bounded before this addition and before any GPU work.
  upload_vertices += overlay_vertices;
  try {
    p.mesh_staging.clear();
    p.mesh_staging.reserve(upload_vertices);
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "viewport mesh staging allocation failed");
  }
  return {};
}

Result<void> WindowsViewport::Impl::prepare_mesh(const RenderFrame &frame, const DrawPrimitive &draw,
                                                 std::size_t i) {
  auto &p = *this;
  auto &range = p.mesh_ranges[i];
  range.first = static_cast<UINT>(p.mesh_staging.size());
  range.count = static_cast<UINT>(draw.geometry->indices().size());
  const std::array eye{frame.camera.eye.x, frame.camera.eye.y, frame.camera.eye.z};
  for (const auto vertex_index : draw.geometry->indices()) {
    const auto &vertex = draw.geometry->vertices()[vertex_index];
    std::array<float, 3> relative{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      // Translation-eye subtraction occurs in double before local addition.
      const auto row = axis * 4;
      const double value = draw.world[row] * vertex.position[0] + draw.world[row + 1] * vertex.position[1] +
                           draw.world[row + 2] * vertex.position[2] + (draw.world[row + 3] - eye[axis]);
      if (!std::isfinite(value) || std::abs(value) > 1e7)
        return fail(ErrorCode::Exhausted, "viewport mesh camera-relative vertex range exceeded");
      relative[axis] = static_cast<float>(value);
    }
    p.mesh_staging.push_back({relative[0], relative[1], relative[2], static_cast<float>(vertex.normal[0]),
                              static_cast<float>(vertex.normal[1]), static_cast<float>(vertex.normal[2]),
                              static_cast<float>(vertex.uv[0]), static_cast<float>(vertex.uv[1])});
  }
  if (draw.selected) {
    range.outline_first = static_cast<UINT>(p.mesh_staging.size());
    range.outline_count = range.count * 2;
    for (UINT face = 0; face < range.count; face += 3)
      for (UINT edge = 0; edge < 3; ++edge) {
        const auto a = p.mesh_staging[range.first + face + edge],
                   b = p.mesh_staging[range.first + face + (edge + 1) % 3];
        p.mesh_staging.push_back(a);
        p.mesh_staging.push_back(b);
      }
  }
  return {};
}

Result<void> WindowsViewport::Impl::prepare_draws(const RenderFrame &frame, bool physical,
                                                  const ResolvedLighting &lighting,
                                                  ViewportVec3 light_direction, const Matrix &vp) {
  auto &p = *this;
  p.mesh_ranges.fill({});
  p.prepared.clear();
  for (std::size_t i = 0; i < frame.draws.size(); ++i) {
    if (!frame.draws[i].id.valid())
      return fail(ErrorCode::InvalidInput, "viewport draw requires stable source ID");
    for (std::size_t j = 0; j < i; ++j)
      if (frame.draws[j].id == frame.draws[i].id)
        return fail(ErrorCode::Duplicate, "duplicate viewport source ID");
    const auto &draw = frame.draws[i];
    if (!valid_color(draw.color))
      return fail(ErrorCode::InvalidInput, "invalid opaque linear viewport color");
    auto color = draw.color;
    // Physical reflectance rho = factor * tint stays within [0,1] per channel; excess is rejected.
    if (physical) {
      auto reflectance = validate_physical_reflectance(
          draw.color, draw.material ? draw.material->base_color() : std::array<float, 4>{1, 1, 1, 1});
      if (!reflectance)
        return reflectance;
    }
    if (draw.material)
      for (std::size_t channel = 0; channel < 3; ++channel)
        color[channel] *= draw.material->base_color()[channel];
    auto copied =
        constants(draw.world, vp, frame, color, light_direction, false, 1, static_cast<bool>(draw.geometry));
    if (!copied)
      return copied.error();
    if (physical) {
      copied.value().light[3] = lighting.irradiance_over_pi;
      copied.value().parameters[0] = lighting.environment;
    }
    copied.value().parameters[3] = p.draw_textures[i]
                                       ? (draw.material->texture_role() == TextureRole::DiffuseReflectanceR
                                              ? 3.f
                                              : static_cast<float>(draw.material->sampling()))
                                       : 0.f;
    p.prepared.push_back(copied.value());
    if (draw.geometry) {
      auto staged = p.prepare_mesh(frame, draw, i);
      if (!staged)
        return staged;
    }
  }
  return {};
}

void WindowsViewport::Impl::prepare_overlays(const RenderFrame &frame) {
  auto &p = *this;
  for (std::size_t i = 0; i < frame.overlays.size(); ++i) {
    const auto &overlay = frame.overlays[i];
    p.overlay_ranges[i] = {static_cast<UINT>(p.mesh_staging.size()),
                           static_cast<UINT>(overlay.vertices.size())};
    p.overlay_prepared[i] = overlay_constants(overlay);
    for (const auto &vertex : overlay.vertices)
      p.mesh_staging.push_back({vertex[0], vertex[1], 0, 0, 0, 1});
  }
}

Result<void> WindowsViewport::Impl::draw_prepared(const RenderFrame &frame, bool physical,
                                                  const ResolvedLighting &lighting,
                                                  ViewportVec3 light_direction, const Matrix &vp,
                                                  ViewportVec3 light, std::size_t overlay_vertices,
                                                  ProfileClock::time_point draw_started) {
  auto &p = *this;
  std::size_t texture_count{}, texture_bytes{};
  auto textures = p.collect_textures(frame, texture_count, texture_bytes);
  if (!textures)
    return textures;
  auto reserved = p.reserve_mesh(frame, overlay_vertices);
  if (!reserved)
    return reserved;
  auto preparation = p.prepare_draws(frame, physical, lighting, light_direction, vp);
  if (!preparation)
    return preparation;
  p.prepare_overlays(frame);
  // Grid follows integer world coordinates around the camera target; its fixed
  // 41x41-line work budget is independent of camera distance or object count.
  std::array<double, 12> grid_world{1, 0, 0, std::floor(frame.camera.target.x), 0, 1, 0, 0,
                                    0, 0, 1, std::floor(frame.camera.target.z)};
  auto grid = constants(grid_world, vp, frame, {.11f, .13f, .17f, 1}, light, true);
  // Ground-grid decoration may be outside the local conversion range at high
  // altitude. Omit that decoration; never reject otherwise valid scene geometry.
  const bool draw_grid = frame.grid && static_cast<bool>(grid);
  const bool draw_x_axis = draw_grid && std::abs(grid_world[11]) <= 20;
  const bool draw_z_axis = draw_grid && std::abs(grid_world[3]) <= 20;
  Constants x_axis{}, z_axis{};
  if (draw_x_axis) {
    auto world = grid_world;
    world[11] = 0;
    auto axis = constants(world, vp, frame, {.45f, .055f, .035f, 1}, light, true);
    if (!axis)
      return axis.error();
    x_axis = axis.value();
  }
  if (draw_z_axis) {
    auto world = grid_world;
    world[3] = 0;
    auto axis = constants(world, vp, frame, {.035f, .18f, .5f, 1}, light, true);
    if (!axis)
      return axis.error();
    z_axis = axis.value();
  }
  const DrawFrame prepared_frame{frame,     physical,      lighting,      grid,
                                 draw_grid, draw_x_axis,   draw_z_axis,   x_axis,
                                 z_axis,    texture_count, texture_bytes, overlay_vertices};
  return p.draw_ready(prepared_frame, draw_started);
}
} // namespace race::render
