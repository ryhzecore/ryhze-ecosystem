#include "race/render/viewport_geometry.hpp"
#include "test.hpp"
#include <limits>

using namespace race;
using namespace race::render;
void viewport_geometry_tests() {
  std::array<GeometryVertex, 3> vertices{{{{0,0,0},{0,0,1}},{{1,0,0},{0,0,1}},{{0,1,0},{0,0,1}}}};
  std::array<std::uint32_t, 3> indices{0,1,2};
  auto packet = ViewportGeometry::create(vertices, indices); CHECK(packet);
  const auto original = vertices;
  vertices[0].position[0] = 5; indices[0] = 2;
  CHECK(packet.value()->vertices()[0] == original[0] && packet.value()->indices()[0] == 0);
  CHECK_ERROR(ViewportGeometry::create(original, indices), ErrorCode::InvalidInput);
  indices = {0,1,3}; CHECK_ERROR(ViewportGeometry::create(original, indices), ErrorCode::InvalidInput);
  indices = {0,1,2};
  CHECK_ERROR(ViewportGeometry::create({}, indices), ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportGeometry::create(original, {}), ErrorCode::InvalidInput);
  for (auto value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e13}) {
    vertices = original; vertices[0].position[0] = value;
    CHECK_ERROR(ViewportGeometry::create(vertices, indices), ErrorCode::InvalidInput);
    vertices = original; vertices[0].normal[0] = value;
    CHECK_ERROR(ViewportGeometry::create(vertices, indices), ErrorCode::InvalidInput);
  }
  vertices = original; vertices[0].normal = {0,0,0}; CHECK(!ViewportGeometry::create(vertices, indices));
  vertices = original; vertices[2].position = {2,0,0}; CHECK(!ViewportGeometry::create(vertices, indices));
  vertices = original; vertices[1].position[0] = 1e-200; vertices[2].position[1] = 1e-200;
  CHECK(ViewportGeometry::create(vertices, indices));
  std::vector<GeometryVertex> large(viewport_mesh_max_vertices, original[0]);
  large[1] = original[1]; large[2] = original[2]; CHECK(ViewportGeometry::create(large, indices));
  large.push_back(original[0]); CHECK_ERROR(ViewportGeometry::create(large, indices), ErrorCode::Exhausted);
  std::vector<std::uint32_t> many(viewport_mesh_max_indices);
  for (std::size_t i=0; i<many.size(); ++i) many[i] = static_cast<std::uint32_t>(i%3);
  CHECK(ViewportGeometry::create(original, many));
  many.push_back(0); CHECK_ERROR(ViewportGeometry::create(original, many), ErrorCode::Exhausted);
}
