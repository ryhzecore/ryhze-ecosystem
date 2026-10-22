#include "race/assets/collision.hpp"
#include "race/assets/mesh.hpp"
#include "race/assets/surface.hpp"
#include "test.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace {
using namespace race;
namespace c=assets::collision;
c::Stamp stamp() {
  c::Stamp value;value.source[0]=std::byte{1};value.settings[0]=std::byte{2};value.importer[0]=std::byte{3};return value;
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,std::size_t width=8) {
  for(std::size_t i=0;i<width;++i)bytes[at+i]=std::byte((value>>(8*i))&255);
}
void resign(std::span<std::byte> bytes) {
  const auto checksum=assets::digest(std::span<const std::byte>(bytes).first(bytes.size()-32));
  std::copy(checksum.begin(),checksum.end(),bytes.end()-32);
}
void roundtrip() {
  c::Primitive geometry{c::Kind::Box,{.1,1,100}};
  const auto encoded=c::encode({1,2},stamp(),geometry);CHECK(encoded);
  const auto& bytes=encoded.value();CHECK(bytes.size()==312);
  auto view=c::validate(bytes);CHECK(view);CHECK(view.value().id()==assets::AssetId{1,2});
  CHECK(view.value().stamp()==stamp());CHECK(view.value().primitive()==geometry);
  CHECK(view.value().bytes().data()==bytes.data());CHECK(view.value().revision()==assets::digest(bytes));
  CHECK(c::encode({1,2},stamp(),geometry).value()==bytes);
  // Independent golden digest is produced with Python struct.pack + hashlib,
  // not by decoding and re-encoding with the implementation under test.
  CHECK(assets::hex(view.value().revision())=="2042a017d79adf153e0797f05f3f813ae1bd480d549dbaa9670f628a46299464");
  CHECK(!assets::mesh::validate(bytes));CHECK(!assets::surface::validate_texture(bytes));CHECK(!assets::surface::validate_material(bytes));
  auto cooked=stamp();cooked.representation=c::Representation::Cooked;cooked.target=1;cooked.cooker[0]=std::byte{4};
  geometry={c::Kind::Sphere,{.1,.1,.1}};
  auto native=c::encode({1,2},cooked,geometry);CHECK(native);auto result=c::validate(native.value());CHECK(result);
  CHECK(result.value().stamp()==cooked);CHECK(result.value().primitive()==geometry);
  CHECK(result.value().revision()!=view.value().revision());
}
void corrupt_wire() {
  const auto good=c::encode({1,2},stamp(),{c::Kind::Box,{1,2,3}}).value();
  for(std::size_t size=0;size<good.size();++size)CHECK(!c::validate(std::span(good).first(size)));
  std::array<std::byte,c::product_bytes+1> trailing{};std::copy(good.begin(),good.end(),trailing.begin());CHECK(!c::validate(trailing));
  for(std::size_t i=0;i<good.size();++i){auto bad=good;bad[i]^=std::byte{1};CHECK(!c::validate(bad));}
  struct Mutation {std::size_t at,width;std::uint64_t value;};
  constexpr Mutation changes[]{
    {8,4,2},{12,4,255},{16,4,1},{20,4,2},{24,4,99},{28,4,1},
    {32,8,0},{40,8,257},{48,8,25},{56,8,1},{240,4,0},{240,4,3},
    {244,4,2},{248,4,23},{252,4,1},
    {256,8,0},{256,8,0x7ff0000000000000ULL},{256,8,0x7ff8000000000000ULL},
    {256,8,0xbff0000000000000ULL},{256,8,0x4059400000000000ULL}, // 101 m
  };
  for(const auto& change:changes){auto bad=good;put(bad,change.at,change.value,change.width);resign(bad);CHECK(!c::validate(bad));}
  auto bad=good;put(bad,64,0);put(bad,72,0);resign(bad);CHECK(!c::validate(bad));
  for(const auto at:{80,112,144}){bad=good;std::fill_n(bad.begin()+at,32,std::byte{});resign(bad);CHECK(!c::validate(bad));}
  bad=good;bad[176]=std::byte{1};resign(bad);CHECK(!c::validate(bad));
  bad=good;bad[208]^=std::byte{1};resign(bad);CHECK_ERROR(c::validate(bad),ErrorCode::InvalidDependency);
  // Relabeling anisotropic box bytes as a sphere cannot bypass shape validation.
  bad=good;put(bad,240,1,4);resign(bad);CHECK_ERROR(c::validate(bad),ErrorCode::Unsupported);
}
void invalid_authoring() {
  const auto provenance=stamp();
  CHECK(!c::encode({},provenance,{}));CHECK(!c::encode({1,2},{},{}));
  for(const double invalid:{0.,-.1,.099,100.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    for(std::size_t axis=0;axis<3;++axis){c::Primitive value;value.dimensions_m[axis]=invalid;CHECK(!c::encode({1,2},provenance,value));}
  }
  CHECK(!c::encode({1,2},provenance,{c::Kind::Sphere,{1,1,2}}));
  CHECK(!c::encode({1,2},provenance,{c::Kind::Sphere,{10.1,10.1,10.1}}));
  CHECK(c::encode({1,2},provenance,{c::Kind::Sphere,{10,10,10}}));
  auto malformed=provenance;malformed.representation=c::Representation::Cooked;malformed.target=1;
  CHECK(!c::encode({1,2},malformed,{}));malformed.cooker[0]=std::byte{1};CHECK(c::encode({1,2},malformed,{}));
  malformed.target=2;CHECK(!c::encode({1,2},malformed,{}));
}
} // namespace
int main(){roundtrip();corrupt_wire();invalid_authoring();std::puts("collision product: golden bytes, shape bounds, provenance and corruption rejection passed");}
