#include "race/assets/mesh.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace race::assets::mesh {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
std::uint64_t integer(std::span<const std::byte> bytes, std::size_t at, std::size_t n = 8) {
  std::uint64_t value{};
  for (std::size_t i = 0; i < n; ++i)
    value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + i])) << (i * 8);
  return value;
}
void put(std::span<std::byte> bytes, std::size_t at, std::uint64_t value, std::size_t n = 8) {
  for (std::size_t i = 0; i < n; ++i) bytes[at + i] = std::byte((value >> (i * 8)) & 255);
}
Digest hash_at(std::span<const std::byte> bytes, std::size_t at) {
  Digest result;
  std::copy_n(bytes.begin() + at, result.size(), result.begin());
  return result;
}
Vertex vertex_at(std::span<const std::byte> bytes, std::size_t index,bool has_uv) {
  Vertex result;
  const auto stride=has_uv?uv_vertex_stride:vertex_stride;
  for (std::size_t i = 0; i < 3; ++i) {
    result.position[i] = std::bit_cast<double>(integer(bytes, header_bytes + index * stride + i * 8));
    result.normal[i] = std::bit_cast<double>(integer(bytes, header_bytes + index * stride + 24 + i * 8));
  }
  if(has_uv)for(std::size_t i=0;i<2;++i)result.uv[i]=std::bit_cast<double>(integer(bytes,header_bytes+index*stride+48+i*8));
  return result;
}
bool valid_vertex(const Vertex &v,bool has_uv) {
  for (std::size_t i = 0; i < 3; ++i)
    if (!std::isfinite(v.position[i]) || std::abs(v.position[i]) > 1e12 || !std::isfinite(v.normal[i]))
      return false;
  const auto length = std::hypot(v.normal[0], v.normal[1], v.normal[2]);
  if(has_uv)for(const auto coordinate:v.uv)if(!std::isfinite(coordinate)||std::abs(coordinate)>1e12)return false;
  return std::isfinite(length) && std::abs(length - 1.0) <= 1e-6;
}
bool triangle(const Vertex &a, const Vertex &b, const Vertex &c) {
  std::array<double, 3> u{}, v{};
  double us{}, vs{};
  for (std::size_t i = 0; i < 3; ++i) {
    u[i] = b.position[i] - a.position[i]; v[i] = c.position[i] - a.position[i];
    us = std::max(us, std::abs(u[i])); vs = std::max(vs, std::abs(v[i]));
  }
  if (us == 0 || vs == 0) return false;
  for (std::size_t i = 0; i < 3; ++i) { u[i] /= us; v[i] /= vs; }
  return std::hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                    u[0] * v[1] - u[1] * v[0]) > 0;
}
Result<std::size_t> size_for(std::size_t vertices, std::size_t indices, const Limits &limits,bool has_uv) {
  if (vertices < 3 || indices < 3 || indices % 3)
    return fail(ErrorCode::InvalidInput, "mesh requires indexed triangles");
  if (vertices > limits.vertices || indices > limits.indices)
    return fail(ErrorCode::Exhausted, "mesh element budget exceeded");
  // Validated hard limits bound both products well below size_t on 32-bit hosts.
  const auto size = header_bytes + vertices * (has_uv?uv_vertex_stride:vertex_stride) + indices * 4 + checksum_bytes;
  if (size > limits.product_bytes) return fail(ErrorCode::Exhausted, "mesh product budget exceeded");
  return size;
}
} // namespace
Result<void> validate_limits(const Limits &l) noexcept {
  if (l.vertices < 3 || l.vertices > 1024 * 1024 || l.indices < 3 || l.indices > 3 * 1024 * 1024 ||
      l.product_bytes < header_bytes + 3 * vertex_stride + 12 + checksum_bytes ||
      l.product_bytes > 128 * 1024 * 1024 || !l.assets || l.assets > 128 ||
      !l.retained_revisions || l.retained_revisions > 4096 || !l.retained_bytes ||
      l.retained_bytes > 1024ULL * 1024 * 1024)
    return fail(ErrorCode::InvalidInput, "invalid native mesh limits");
  return {};
}
Result<void> validate_stamp(const Stamp &s) noexcept {
  if (s.source == Digest{} || s.settings == Digest{} || s.importer == Digest{})
    return fail(ErrorCode::InvalidInput, "mesh provenance fingerprint missing");
  if (s.dependencies != digest({}))
    return fail(ErrorCode::Unsupported, "mesh schemas 1 and 2 have no external dependencies");
  if (s.representation == Representation::Imported) {
    if (s.target || s.cooker != Digest{})
      return fail(ErrorCode::IncompatibleVersion, "imported mesh cannot carry a cooked target");
  } else if (s.representation == Representation::Cooked) {
    if (s.target != portable_cpu_target || s.cooker == Digest{})
      return fail(ErrorCode::IncompatibleVersion, "unsupported mesh cooked profile");
  } else return fail(ErrorCode::IncompatibleVersion, "unknown mesh representation");
  return {};
}
View::View(AssetId id, Stamp stamp, Digest revision, Bounds bounds, std::size_t vertices,
           std::size_t indices, std::span<const std::byte> bytes,bool has_uv)
    : id_(id), stamp_(stamp), revision_(revision), bounds_(bounds), vertices_(vertices),
      indices_(indices), has_uv_(has_uv), bytes_(bytes) {}
Result<Vertex> View::vertex(std::size_t index) const noexcept {
  if (index >= vertices_) return fail(ErrorCode::InvalidInput, "mesh vertex index out of range");
  return vertex_at(bytes_, index,has_uv_);
}
Result<std::uint32_t> View::index(std::size_t index) const noexcept {
  if (index >= indices_) return fail(ErrorCode::InvalidInput, "mesh index offset out of range");
  return static_cast<std::uint32_t>(integer(bytes_, header_bytes + vertices_ * (has_uv_?uv_vertex_stride:vertex_stride) + index * 4, 4));
}
Result<View> validate(std::span<const std::byte> bytes, const Limits &limits) {
  auto admitted = validate_limits(limits); if (!admitted) return admitted.error();
  if (bytes.size() > limits.product_bytes) return fail(ErrorCode::Exhausted, "mesh product budget exceeded");
  if (bytes.size() < header_bytes + checksum_bytes || std::memcmp(bytes.data(), "RACEAST1", 8))
    return fail(ErrorCode::InvalidInput, "invalid native asset signature or size");
  const auto schema=integer(bytes,20,4);const bool has_uv=schema==2;
  const auto stride=has_uv?uv_vertex_stride:vertex_stride;
  if (integer(bytes, 8, 4) != 1 || integer(bytes, 12, 4) != header_bytes ||
      integer(bytes, 16, 4) != 1 || (schema!=1&&schema!=2) ||
      integer(bytes, 240, 4) != stride || integer(bytes, 244, 4) != 4 || integer(bytes, 248) != 0)
    return fail(ErrorCode::IncompatibleVersion, "unsupported native mesh format or features");
  if (integer(bytes, 32) != bytes.size() || integer(bytes, 40) != header_bytes)
    return fail(ErrorCode::InvalidInput, "noncanonical native mesh bounds");
  const auto vc = integer(bytes, 48), ic = integer(bytes, 56);
  if (vc > limits.vertices || ic > limits.indices)
    return fail(ErrorCode::Exhausted, "mesh element budget exceeded");
  auto size = size_for(static_cast<std::size_t>(vc), static_cast<std::size_t>(ic), limits,has_uv);
  if (!size) return size.error();
  if (bytes.size() != size.value()) return fail(ErrorCode::InvalidInput, "native mesh size mismatch or trailing bytes");
  AssetId id{integer(bytes, 64), integer(bytes, 72)};
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "mesh asset identity is zero");
  Stamp stamp;
  stamp.representation = static_cast<Representation>(integer(bytes, 24, 4));
  stamp.target = static_cast<std::uint32_t>(integer(bytes, 28, 4));
  stamp.source = hash_at(bytes, 80); stamp.settings = hash_at(bytes, 112);
  stamp.importer = hash_at(bytes, 144); stamp.cooker = hash_at(bytes, 176);
  stamp.dependencies = hash_at(bytes, 208);
  auto provenance = validate_stamp(stamp); if (!provenance) return provenance.error();
  if (digest(bytes.first(bytes.size() - checksum_bytes)) != hash_at(bytes, bytes.size() - checksum_bytes))
    return fail(ErrorCode::InvalidInput, "native mesh checksum mismatch");
  Bounds bounds{vertex_at(bytes, 0,has_uv).position, vertex_at(bytes, 0,has_uv).position};
  for (std::size_t i = 0; i < vc; ++i) {
    const auto v = vertex_at(bytes, i,has_uv);
    if (!valid_vertex(v,has_uv)) return fail(ErrorCode::InvalidInput, "invalid mesh position, normal or UV");
    for (std::size_t axis = 0; axis < 3; ++axis) {
      bounds.minimum[axis] = std::min(bounds.minimum[axis], v.position[axis]);
      bounds.maximum[axis] = std::max(bounds.maximum[axis], v.position[axis]);
    }
  }
  const auto index_start = header_bytes + static_cast<std::size_t>(vc) * stride;
  for (std::size_t i = 0; i < ic; i += 3) {
    const auto a = integer(bytes, index_start + i * 4, 4), b = integer(bytes, index_start + (i + 1) * 4, 4),
               c = integer(bytes, index_start + (i + 2) * 4, 4);
    if (a >= vc || b >= vc || c >= vc || a == b || b == c || a == c)
      return fail(ErrorCode::InvalidInput, "invalid mesh triangle index");
    if (!triangle(vertex_at(bytes, static_cast<std::size_t>(a),has_uv), vertex_at(bytes, static_cast<std::size_t>(b),has_uv),
                  vertex_at(bytes, static_cast<std::size_t>(c),has_uv)))
      return fail(ErrorCode::InvalidInput, "degenerate mesh triangle");
  }
  return View(id, stamp, digest(bytes), bounds, static_cast<std::size_t>(vc), static_cast<std::size_t>(ic), bytes,has_uv);
}
Result<std::vector<std::byte>> encode(AssetId id, const Stamp &stamp, std::span<const Vertex> vertices,
                                    std::span<const std::uint32_t> indices, const Limits &limits,bool has_uv) {
  auto admitted = validate_limits(limits); if (!admitted) return admitted.error();
  auto provenance = validate_stamp(stamp); if (!provenance) return provenance.error();
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "mesh asset identity is zero");
  auto size = size_for(vertices.size(), indices.size(), limits,has_uv); if (!size) return size.error();
  for (const auto &v : vertices)
    if (!valid_vertex(v,has_uv)) return fail(ErrorCode::InvalidInput, "invalid mesh position, normal or UV");
  for (std::size_t i = 0; i < indices.size(); i += 3) {
    const auto a = indices[i], b = indices[i + 1], c = indices[i + 2];
    if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size() || a == b || b == c || a == c)
      return fail(ErrorCode::InvalidInput, "invalid mesh triangle index");
    if (!triangle(vertices[a], vertices[b], vertices[c])) return fail(ErrorCode::InvalidInput, "degenerate mesh triangle");
  }
  try {
    const auto stride=has_uv?uv_vertex_stride:vertex_stride;
    std::vector<std::byte> bytes(size.value());
    std::memcpy(bytes.data(), "RACEAST1", 8);
    put(bytes, 8, 1, 4); put(bytes, 12, header_bytes, 4); put(bytes, 16, 1, 4); put(bytes, 20, has_uv?2:1, 4);
    put(bytes, 24, static_cast<std::uint32_t>(stamp.representation), 4); put(bytes, 28, stamp.target, 4);
    put(bytes, 32, bytes.size()); put(bytes, 40, header_bytes); put(bytes, 48, vertices.size());
    put(bytes, 56, indices.size()); put(bytes, 64, id.high); put(bytes, 72, id.low);
    const std::array hashes{stamp.source, stamp.settings, stamp.importer, stamp.cooker, stamp.dependencies};
    for (std::size_t i = 0; i < hashes.size(); ++i) std::copy(hashes[i].begin(), hashes[i].end(), bytes.begin() + 80 + i * 32);
    put(bytes, 240, stride, 4); put(bytes, 244, 4, 4);
    for (std::size_t i = 0; i < vertices.size(); ++i){
      for (std::size_t axis = 0; axis < 3; ++axis) {
        put(bytes, header_bytes + i * stride + axis * 8, std::bit_cast<std::uint64_t>(vertices[i].position[axis]));
        put(bytes, header_bytes + i * stride + 24 + axis * 8, std::bit_cast<std::uint64_t>(vertices[i].normal[axis]));
      }
      if(has_uv)for(std::size_t axis=0;axis<2;++axis)
        put(bytes,header_bytes+i*stride+48+axis*8,std::bit_cast<std::uint64_t>(vertices[i].uv[axis]));
    }
    for (std::size_t i = 0; i < indices.size(); ++i) put(bytes, header_bytes + vertices.size() * stride + i * 4, indices[i], 4);
    const auto checksum = digest(std::span<const std::byte>(bytes).first(bytes.size() - checksum_bytes));
    std::copy(checksum.begin(), checksum.end(), bytes.end() - checksum_bytes);
    return bytes;
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "mesh encode allocation failed"); }
}
} // namespace race::assets::mesh
