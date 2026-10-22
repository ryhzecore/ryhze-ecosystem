#include "mesh_pipeline.hpp"
#include "assets_support.hpp"
#include <algorithm>
#include <cmath>

void asset_fail_after(std::size_t);
void asset_fail_disable();
namespace {
namespace p = race::mesh_pipeline;
namespace m = race::assets::mesh;
constexpr std::string_view triangle = "# triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
auto bytes(std::string_view text) { return std::as_bytes(std::span(text)); }
auto imported(std::string_view text = triangle, const p::Limits &limits = {}) {
  return p::import_obj({77, 1}, bytes(text), limits);
}
void explicit_profiles() {
  constexpr std::string_view mapped="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
  const auto legacy=imported();CHECK(legacy);
  auto checked=p::import_obj(aid(1),bytes(triangle),p::Profile::Geometry);CHECK(checked&&checked.value()==legacy.value());
  const auto automatic=imported(mapped);CHECK(automatic);
  auto uv=p::import_obj(aid(1),bytes(mapped),p::Profile::GeometryUv);CHECK(uv&&uv.value()==automatic.value());
  auto view=m::validate(uv.value());CHECK(view&&view.value().has_uv());
  const auto stamp=p::profile_import_stamp(p::Profile::GeometryUv,assets::digest(bytes(mapped)));CHECK(stamp&&view.value().stamp()==stamp.value());
  CHECK(p::profile_import_stamp(p::Profile::Geometry,assets::digest(bytes(triangle))).value()==p::import_stamp(assets::digest(bytes(triangle))));
  // Cross-profile rejection occurs before reserve/encode, not after publication.
  asset_fail_after(0);auto plain_rejected=p::import_obj(aid(1),bytes(triangle),p::Profile::GeometryUv);asset_fail_disable();
  CHECK_ERROR(plain_rejected,ErrorCode::IncompatibleVersion);
  asset_fail_after(0);auto uv_rejected=p::import_obj(aid(1),bytes(mapped),p::Profile::Geometry);asset_fail_disable();
  CHECK_ERROR(uv_rejected,ErrorCode::IncompatibleVersion);
  for(auto value:{0U,3U,255U}){
    const auto profile=static_cast<p::Profile>(value);
    CHECK_ERROR(p::profile_import_stamp(profile,{}),ErrorCode::IncompatibleVersion);
    CHECK_ERROR(p::import_obj(aid(1),bytes(mapped),profile),ErrorCode::IncompatibleVersion);
  }
  auto cooked=p::cook(uv.value());CHECK(cooked);
  CHECK(m::validate(cooked.value()).value().stamp()==p::cook_stamp(stamp.value()));
}
void parser() {
  auto product = imported(); CHECK(product && imported().value() == product.value());
  auto view = m::validate(product.value()); CHECK(view);
  CHECK(view.value().id() == PersistentId{77, 1});
  CHECK(view.value().stamp().source == assets::digest(bytes(triangle)));
  CHECK(view.value().vertex_count() == 3 && view.value().index_count() == 3);
  CHECK(view.value().vertex(0).value().normal == std::array<double, 3>{0, 0, 1});
  CHECK(view.value().vertex(1).value().position == std::array<double, 3>{1, 0, 0});
  auto crlf = imported("\t# comment\r\nv +0 -0 0\r\nv 1e0 0 0\r\nv 0 .1e1 0\r\nf 1 2 3 # end");
  CHECK(crlf);
  auto other = m::validate(crlf.value()); CHECK(other);
  CHECK(other.value().revision() != view.value().revision());
  CHECK(other.value().stamp().settings == view.value().stamp().settings);
  for (std::size_t i = 0; i < 3; ++i) CHECK(other.value().vertex(i).value() == view.value().vertex(i).value());
  CHECK(imported("v 0 0 0\nv 1e-200 0 0\nv 0 1e-200 0\nf 1 2 3"));
  CHECK(imported("v 1000000000000 0 0\nv 999999999999 0 0\nv 1000000000000 1 0\nf 1 2 3"));
  CHECK(!p::import_obj({}, bytes(triangle)));
  for (auto suffix : {"f 0 2 3", "f -1 2 3", "f 1 2 4", "f 1 1 3", "f 1 2", "f 1 2 3 1",
                     "f 1/1 2/2 3/3", "f 1//1 2//2 3//3", "f 4294967296 2 3", "f 1x 2 3",
                     "vt 0 0", "vn 0 0 1", "mtllib other.mtl", "usemtl other", "call other.obj",
                     "o triangle", "s off", "v 0 0 0 1"}) {
    std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"; text += suffix;
    CHECK(!imported(text));
  }
  for (auto number : {"nan", "inf", "-inf", "1e9999", "1e-9999", "1000000000001", "0x1p0", "+-1", "++1", "1z"}) {
    std::string text = "v "; text += number; text += " 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3";
    CHECK(!imported(text));
  }
  for (auto text : {"", "# only comment", "f 1 2 3\nv 0 0 0\nv 1 0 0\nv 0 1 0",
                    "v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3", "v 0 0 0\rv 1 0 0",
                    "#\vbad", "#\x7f", "#\xc3\xa9"}) CHECK(!imported(text));
  std::string nul(triangle); nul.push_back('\0'); CHECK(!imported(nul));
  // Every proper prefix that reaches a complete face is valid; all earlier ones reject.
  const auto face = triangle.find("f 1 2 3") + 7;
  for (std::size_t i = 0; i < face; ++i) CHECK(!imported(triangle.substr(0, i)));
}
void limits() {
  p::Limits l;
  l.source_bytes = triangle.size(); CHECK(imported(triangle, l));
  --l.source_bytes; CHECK_ERROR(imported(triangle, l), ErrorCode::Exhausted);
  l = {}; l.line_bytes = 10; l.token_bytes = 10; CHECK(imported(triangle, l));
  l.line_bytes = 9; l.token_bytes = 9; CHECK_ERROR(imported(triangle, l), ErrorCode::Exhausted);
  l = {}; l.token_bytes = 1;
  CHECK(imported("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3", l));
  CHECK_ERROR(imported("v 00 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3", l), ErrorCode::Exhausted);
  l = {}; l.source_vertices = 3; CHECK(imported(triangle, l));
  CHECK_ERROR(imported(std::string(triangle) + "v 0 0 1", l), ErrorCode::Exhausted);
  const auto two = std::string(triangle) + "f 1 3 2\n";
  l = {}; l.triangles = 1; CHECK_ERROR(imported(two, l), ErrorCode::Exhausted);
  l = {}; l.mesh.vertices = 3; CHECK_ERROR(imported(two, l), ErrorCode::Exhausted);
  l = {}; l.mesh.indices = 3; CHECK_ERROR(imported(two, l), ErrorCode::Exhausted);
  l = {}; l.mesh.product_bytes = 444; CHECK(imported(triangle, l));
  CHECK_ERROR(imported(two, l), ErrorCode::Exhausted);
  l = {}; l.source_bytes = SIZE_MAX; CHECK_ERROR(imported(triangle, l), ErrorCode::InvalidInput);
  l = {}; l.mesh.product_bytes = 1; CHECK_ERROR(imported(triangle, l), ErrorCode::InvalidInput);
  l = {}; l.triangles = 0; CHECK_ERROR(imported(triangle, l), ErrorCode::InvalidInput);
  // Oversized/malformed input is rejected before the first dynamic allocation.
  l = {}; l.source_bytes = 1;
  asset_fail_after(0); auto no_allocation = imported(triangle, l); asset_fail_disable();
  CHECK_ERROR(no_allocation, ErrorCode::Exhausted);
  asset_fail_after(0); auto no_parse_allocation = imported("mtllib forbidden.mtl"); asset_fail_disable();
  CHECK_ERROR(no_parse_allocation, ErrorCode::Unsupported);
}
void cooking_publication() {
  auto input = imported(); CHECK(input);
  auto cooked = p::cook(input.value()); CHECK(cooked && p::cook(input.value()).value() == cooked.value());
  auto a = m::validate(input.value()), b = m::validate(cooked.value()); CHECK(a && b);
  CHECK(a.value().id() == b.value().id());
  CHECK(b.value().stamp().representation == m::Representation::Cooked);
  CHECK(b.value().stamp().target == m::portable_cpu_target);
  CHECK(b.value().stamp().source == a.value().stamp().source);
  CHECK(b.value().stamp().settings == a.value().stamp().settings);
  CHECK(b.value().stamp().importer == a.value().stamp().importer);
  CHECK(b.value().stamp().cooker != assets::Digest{});
  CHECK(b.value().revision() != a.value().revision());
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(a.value().vertex(i).value() == b.value().vertex(i).value());
    CHECK(a.value().index(i).value() == b.value().index(i).value());
  }
  CHECK_ERROR(p::cook(cooked.value()), ErrorCode::InvalidInput);
  auto corrupt = input.value(); corrupt.back() ^= std::byte{1}; CHECK(!p::cook(corrupt));
  FakeFiles files;
  auto first = p::publish(files, input.value()); CHECK(first);
  CHECK(first.value() == assets::hex(a.value().revision()) + ".rasset");
  CHECK(platform::valid_content_leaf(first.value()));
  CHECK(p::publish(files, input.value()).value() == first.value());
  const auto before = files.data;
  CHECK(!p::publish(files, corrupt)); CHECK(files.data == before);
  files.fail_write = files.writes;
  CHECK_ERROR(p::publish(files, cooked.value()), ErrorCode::IoFailure); CHECK(files.data == before);
  files.fail_write = SIZE_MAX;
  auto second = p::publish(files, cooked.value()); CHECK(second && first.value() != second.value());
  CHECK(files.data.at(first.value()) == input.value() && files.data.at(second.value()) == cooked.value());
  files.data[second.value()] = input.value();
  CHECK_ERROR(p::publish(files, cooked.value()), ErrorCode::Duplicate);
  CHECK(files.data.at(second.value()) == input.value());
  struct CorruptReadback final : FakeFiles {
    Result<std::vector<std::byte>> read(std::string_view n, std::size_t limit) override {
      auto read = FakeFiles::read(n, limit);
      if (read && !read.value().empty()) read.value()[0] ^= std::byte{1};
      return read;
    }
  } corrupt_files;
  CHECK_ERROR(p::publish(corrupt_files, input.value()), ErrorCode::IoFailure);
  CHECK(corrupt_files.data.at(first.value()) == input.value());
}
void faults() {
  auto input = imported(); CHECK(input);
  bool imported_ok{}, cooked_ok{}, published_ok{};
  for (std::size_t n = 0; n < 32 && !imported_ok; ++n) {
    asset_fail_after(n); auto result = imported(); asset_fail_disable();
    if (result) { CHECK(result.value() == input.value()); imported_ok = true; }
    else CHECK(result.error().code == ErrorCode::Exhausted);
  }
  for (std::size_t n = 0; n < 32 && !cooked_ok; ++n) {
    asset_fail_after(n); auto result = p::cook(input.value()); asset_fail_disable();
    if (result) cooked_ok = true; else CHECK(result.error().code == ErrorCode::Exhausted);
  }
  // The real rooted writer is transactional; this fault double throws before
  // publication, avoiding std::map's intentionally nontransactional operator[].
  struct FailingFiles final : FakeFiles {
    Result<void> publish_immutable(std::string_view, std::span<const std::byte>) override { throw std::bad_alloc(); }
  } failing;
  CHECK_ERROR(p::publish(failing, input.value()), ErrorCode::Exhausted); CHECK(failing.data.empty());
  FakeFiles files; CHECK(p::publish(files, input.value())); const auto before = files.data;
  for (std::size_t n = 0; n < 32 && !published_ok; ++n) {
    asset_fail_after(n); auto result = p::publish(files, input.value()); asset_fail_disable();
    if (result) published_ok = true; else CHECK(result.error().code == ErrorCode::Exhausted);
    CHECK(files.data == before);
  }
  CHECK(imported_ok && cooked_ok && published_ok);
}
} // namespace
void mesh_pipeline_tests() { parser(); explicit_profiles(); limits(); cooking_publication(); faults(); }
