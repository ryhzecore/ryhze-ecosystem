#include "mesh_pipeline.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace race::mesh_pipeline {
namespace {
namespace m = assets::mesh;
using Position = std::array<double, 3>;
using Texcoord = std::array<double, 2>;
assets::Digest hash(std::string_view text) {
  return assets::digest(std::as_bytes(std::span(text)));
}
assets::mesh::Stamp uv_import_stamp(assets::Digest source) noexcept {
  m::Stamp stamp;stamp.source=source;
  stamp.settings=hash("race-obj-triangles-uv-settings-2:metres;right-handed;y-up;ccw;flat-normals;binary64;vt-double2;top-left-v=1-v");
  stamp.importer=hash("race-first-party-obj-triangles-uv-importer-2");return stamp;
}
struct Counts { std::size_t vertices{}, faces{},texcoords{};bool has_uv{}; };
struct Line {
  std::array<std::string_view, 4> tokens{};
  std::size_t count{};
};
Result<Line> tokenize(std::string_view text, const Limits &limits) {
  if (text.size() > limits.line_bytes)
    return fail(ErrorCode::Exhausted, "OBJ line byte limit");
  // Validate even ignored comments. No escapes, continuations, NUL or Unicode.
  for (unsigned char c : text)
    if ((c < 32 && c != '\t' && c != '\r') || c > 126)
      return fail(ErrorCode::InvalidInput, "OBJ requires bounded ASCII text");
  if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
  if (text.find('\r') != text.npos)
    return fail(ErrorCode::InvalidInput, "OBJ carriage return must end a line");
  text = text.substr(0, text.find('#'));
  Line result;
  while (!text.empty()) {
    auto first = text.find_first_not_of(" \t");
    if (first == text.npos) break;
    text.remove_prefix(first);
    auto end = text.find_first_of(" \t");
    auto token = text.substr(0, end);
    if (token.size() > limits.token_bytes)
      return fail(ErrorCode::Exhausted, "OBJ token byte limit");
    if (result.count == result.tokens.size())
      return fail(ErrorCode::Unsupported, "OBJ profile accepts triangle faces, xyz vertices and uv texture coordinates only");
    result.tokens[result.count++] = token;
    if (end == text.npos) break;
    text.remove_prefix(end);
  }
  return result;
}
Result<double> coordinate(std::string_view token) {
  // Decimal only; from_chars is locale independent. Explicit '+' is permitted.
  if (!token.empty() && token.front() == '+') {
    token.remove_prefix(1);
    if (!token.empty() && (token.front() == '-' || token.front() == '+'))
      return fail(ErrorCode::InvalidInput, "OBJ repeated coordinate sign");
  }
  if (token.empty()) return fail(ErrorCode::InvalidInput, "OBJ empty coordinate");
  double value{};
  auto parsed = std::from_chars(token.data(), token.data() + token.size(), value,
                                std::chars_format::general);
  if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
      !std::isfinite(value) || std::abs(value) > 1e12)
    return fail(ErrorCode::InvalidInput, "OBJ invalid or out-of-range coordinate");
  return value == 0 ? 0.0 : value; // Canonicalize signed zero in geometry only.
}
Result<std::size_t> vertex_index(std::string_view token, std::size_t count) {
  std::uint32_t value{};
  auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
      !value || value > count)
    return fail(ErrorCode::InvalidInput, "OBJ index must name a preceding vertex");
  return static_cast<std::size_t>(value - 1);
}
Result<Position> normal(const Position &a, const Position &b, const Position &c) {
  Position u{}, v{}, n{};
  double us{}, vs{};
  for (std::size_t i = 0; i < 3; ++i) {
    u[i] = b[i] - a[i]; v[i] = c[i] - a[i];
    us = std::max(us, std::abs(u[i])); vs = std::max(vs, std::abs(v[i]));
  }
  if (!us || !vs) return fail(ErrorCode::InvalidInput, "OBJ degenerate triangle");
  for (std::size_t i = 0; i < 3; ++i) { u[i] /= us; v[i] /= vs; }
  n = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
       u[0] * v[1] - u[1] * v[0]};
  const auto length = std::hypot(n[0], n[1], n[2]);
  if (!length) return fail(ErrorCode::InvalidInput, "OBJ degenerate triangle");
  for (auto &value : n) value /= length;
  return n;
}
// First pass has no dynamic allocations; admits all cardinalities before reserve.
// Second pass reads the same immutable input and emits exact bounded arrays.
Result<Counts> parse(std::string_view text, const Limits &limits,
                     std::vector<Position> *positions = nullptr,
                     std::vector<m::Vertex> *vertices = nullptr,
                     std::vector<std::uint32_t> *indices = nullptr,
                     std::vector<Texcoord> *texcoords = nullptr) {
  Counts counts;
  while (!text.empty()) {
    auto end = text.find('\n');
    auto parsed = tokenize(text.substr(0, end), limits);
    if (!parsed) return parsed.error();
    const auto &line = parsed.value();
    if (line.count) {
      if (line.tokens[0] != "v" && line.tokens[0] != "f"&&line.tokens[0]!="vt")
        return fail(ErrorCode::Unsupported, "unsupported OBJ directive or external reference");
      if (line.count != (line.tokens[0]=="vt"?3U:4U)) return fail(ErrorCode::InvalidInput, "OBJ operand count does not match directive");
      if (line.tokens[0] == "v") {
        if (++counts.vertices > limits.source_vertices)
          return fail(ErrorCode::Exhausted, "OBJ source vertex limit");
        Position p{};
        for (std::size_t i = 0; i < 3; ++i) {
          auto value = coordinate(line.tokens[i + 1]);
          if (!value) return value.error();
          p[i] = value.value();
        }
        if (positions) positions->push_back(p);
      } else if(line.tokens[0]=="vt"){
        if(++counts.texcoords>limits.source_vertices)return fail(ErrorCode::Exhausted,"OBJ texture coordinate limit");
        Texcoord uv{};for(std::size_t i=0;i<2;++i){auto value=coordinate(line.tokens[i+1]);if(!value)return value.error();uv[i]=value.value();}
        uv[1]=1-uv[1];if(uv[1]==0)uv[1]=0;
        if(!std::isfinite(uv[1])||std::abs(uv[1])>1e12)return fail(ErrorCode::InvalidInput,"OBJ converted texture coordinate out of range");
        if(texcoords)texcoords->push_back(uv);
      } else {
        const bool mapped=line.tokens[1].find('/')!=std::string_view::npos;
        if(counts.faces&&mapped!=counts.has_uv)return fail(ErrorCode::Unsupported,"OBJ cannot mix mapped and unmapped faces");
        counts.has_uv=mapped;
        const auto stride=mapped?m::uv_vertex_stride:m::vertex_stride;
        if (++counts.faces > limits.triangles || counts.faces > limits.mesh.vertices / 3 ||
            counts.faces > limits.mesh.indices / 3 ||
            counts.faces > (limits.mesh.product_bytes - m::header_bytes - m::checksum_bytes) /
                               (3 * (stride + 4)))
          return fail(ErrorCode::Exhausted, "OBJ triangle or expanded product limit");
        std::array<std::size_t, 3> face{},texture_face{};
        for (std::size_t i = 0; i < 3; ++i) {
          const auto token=line.tokens[i+1];const auto slash=token.find('/');
          if((slash!=token.npos)!=mapped)return fail(ErrorCode::Unsupported,"OBJ face UV mapping must be complete");
          if(mapped){
            if(token.find('/',slash+1)!=token.npos)return fail(ErrorCode::Unsupported,"OBJ normal indices are unsupported");
            auto texture=vertex_index(token.substr(slash+1),counts.texcoords);if(!texture)return texture.error();texture_face[i]=texture.value();
          }
          auto index = vertex_index(token.substr(0,slash), counts.vertices);
          if (!index) return index.error();
          face[i] = index.value();
        }
        if (face[0] == face[1] || face[0] == face[2] || face[1] == face[2])
          return fail(ErrorCode::InvalidInput, "OBJ repeated triangle index");
        if (positions) {
          auto n = normal((*positions)[face[0]], (*positions)[face[1]], (*positions)[face[2]]);
          if (!n) return n.error();
          for (std::size_t i=0;i<face.size();++i) {
            indices->push_back(static_cast<std::uint32_t>(vertices->size()));
            vertices->push_back({(*positions)[face[i]], n.value(),mapped?(*texcoords)[texture_face[i]]:Texcoord{}});
          }
        }
      }
    }
    if (end == text.npos) break;
    text.remove_prefix(end + 1);
  }
  if (!counts.faces) return fail(ErrorCode::InvalidInput, "OBJ contains no triangles");
  if(counts.texcoords&&!counts.has_uv)return fail(ErrorCode::Unsupported,"OBJ texture coordinates require complete mapped faces");
  return counts;
}
} // namespace
Result<void> validate_limits(const Limits &l) noexcept {
  auto mesh = m::validate_limits(l.mesh); if (!mesh) return mesh;
  if (!l.source_bytes || l.source_bytes > 64 * 1024 * 1024 ||
      !l.line_bytes || l.line_bytes > 4096 || !l.token_bytes || l.token_bytes > 128 ||
      l.token_bytes > l.line_bytes || l.source_vertices < 3 || l.source_vertices > 1024 * 1024 ||
      !l.triangles || l.triangles > 1024 * 1024)
    return fail(ErrorCode::InvalidInput, "invalid OBJ import limits");
  return {};
}
assets::mesh::Stamp import_stamp(assets::Digest source) noexcept {
  m::Stamp stamp;
  stamp.source = source;
  stamp.settings = hash("race-obj-triangles-settings-1:metres;right-handed;y-up;ccw;flat-normals;binary64");
  stamp.importer = hash("race-first-party-obj-triangles-importer-1");
  return stamp;
}
Result<assets::mesh::Stamp> profile_import_stamp(Profile profile,assets::Digest source) noexcept {
  if(profile==Profile::Geometry)return import_stamp(source);
  if(profile==Profile::GeometryUv)return uv_import_stamp(source);
  return fail(ErrorCode::IncompatibleVersion,"Unknown compiled OBJ profile");
}
static Result<std::vector<std::byte>> import_obj_impl(assets::AssetId id,std::span<const std::byte> bytes,
                                        const Limits &limits,const Profile* expected) {
  auto admitted = validate_limits(limits); if (!admitted) return admitted.error();
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "OBJ asset identity is zero");
  if (bytes.empty()) return fail(ErrorCode::InvalidInput, "OBJ source is empty");
  if (bytes.size() > limits.source_bytes) return fail(ErrorCode::Exhausted, "OBJ source byte limit");
  const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  auto counts = parse(text, limits); if (!counts) return counts.error();
  if(expected&&counts.value().has_uv!=(*expected==Profile::GeometryUv))
    return fail(ErrorCode::IncompatibleVersion,"OBJ mapping does not match the explicit import profile");
  try {
    std::vector<Position> positions; positions.reserve(counts.value().vertices);
    std::vector<Texcoord> texcoords;texcoords.reserve(counts.value().texcoords);
    std::vector<m::Vertex> vertices; vertices.reserve(counts.value().faces * 3);
    std::vector<std::uint32_t> indices; indices.reserve(counts.value().faces * 3);
    auto parsed = parse(text, limits, &positions, &vertices, &indices,&texcoords);
    if (!parsed) return parsed.error();
    const auto source=assets::digest(bytes);
    const auto stamp = counts.value().has_uv?uv_import_stamp(source):import_stamp(source);
    return m::encode(id, stamp, vertices, indices, limits.mesh,counts.value().has_uv);
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "OBJ import allocation"); }
}
Result<std::vector<std::byte>> import_obj(assets::AssetId id,std::span<const std::byte> bytes,const Limits& limits) {
  return import_obj_impl(id,bytes,limits,nullptr);
}
Result<std::vector<std::byte>> import_obj(assets::AssetId id,std::span<const std::byte> bytes,Profile profile,const Limits& limits) {
  auto valid=profile_import_stamp(profile,{});if(!valid)return valid.error();
  return import_obj_impl(id,bytes,limits,&profile);
}
Result<std::vector<std::byte>> cook(std::span<const std::byte> bytes, const m::Limits &limits) {
  auto decoded = m::validate(bytes, limits); if (!decoded) return decoded.error();
  const auto &view = decoded.value();
  if (view.stamp().representation != m::Representation::Imported)
    return fail(ErrorCode::InvalidInput, "mesh cooking requires imported native input");
  try {
    std::vector<m::Vertex> vertices; vertices.reserve(view.vertex_count());
    std::vector<std::uint32_t> indices; indices.reserve(view.index_count());
    for (std::size_t i = 0; i < view.vertex_count(); ++i) vertices.push_back(view.vertex(i).value());
    for (std::size_t i = 0; i < view.index_count(); ++i) indices.push_back(view.index(i).value());
    auto stamp = cook_stamp(view.stamp());
    if(view.has_uv())stamp.cooker=hash("race-native-mesh-uv-portable-cpu-cooker-2");
    return m::encode(view.id(), stamp, vertices, indices, limits,view.has_uv());
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "mesh cook allocation"); }
}
assets::mesh::Stamp cook_stamp(assets::mesh::Stamp stamp) noexcept {
  stamp.representation=m::Representation::Cooked;stamp.target=m::portable_cpu_target;
  const auto uv=uv_import_stamp(stamp.source);
  stamp.cooker=hash(stamp.settings==uv.settings&&stamp.importer==uv.importer?
      "race-native-mesh-uv-portable-cpu-cooker-2":"race-native-mesh-portable-cpu-cooker-1");return stamp;
}
Result<std::string> publish(platform::RootedFiles &files, std::span<const std::byte> bytes,
                            const m::Limits &limits) {
  auto decoded = m::validate(bytes, limits); if (!decoded) return decoded.error();
  try {
    auto name = assets::hex(decoded.value().revision()) + ".rasset";
    auto written = files.publish_immutable(name, bytes); if (!written) return written.error();
    auto readback = files.read(name, limits.product_bytes); if (!readback) return readback.error();
    if (!std::equal(bytes.begin(), bytes.end(), readback.value().begin(), readback.value().end()))
      return fail(ErrorCode::IoFailure, "mesh publication readback mismatch");
    return name;
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "mesh publish allocation"); }
}
} // namespace race::mesh_pipeline
