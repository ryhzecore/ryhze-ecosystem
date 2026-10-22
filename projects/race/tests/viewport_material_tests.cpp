#include "race/render/viewport_material.hpp"
#include "race/render/viewport_geometry.hpp"
#include "test.hpp"
#include <limits>

using namespace race;
using namespace race::render;
void mip_packet_contracts() {
  // Independent NPOT layout: 3x5, 1x2, 1x1. Every span borrows the copied owner.
  std::vector<std::byte> texels(72,std::byte{255}); texels[60]=std::byte{128};
  auto chain=ViewportTexture::create_mips(3,5,texels); CHECK(chain);
  CHECK(chain.value()->complete_mip_chain() && chain.value()->mip_count()==3);
  CHECK(chain.value()->pixels().size()==60 && chain.value()->texels().size()==72);
  const auto middle=chain.value()->mip(1),last=chain.value()->mip(2);
  CHECK(middle && middle->width==1 && middle->height==2 && middle->pixels.size()==8);
  CHECK(middle->pixels.data()==chain.value()->texels().data()+60);
  CHECK(last && last->width==1 && last->height==1 && last->pixels.size()==4);
  CHECK(last->pixels.data()==chain.value()->texels().data()+68);
  CHECK(!chain.value()->mip(3) && !chain.value()->mip(UINT32_MAX));
  texels[60]=std::byte{0}; CHECK(middle->pixels[0]==std::byte{128});
  auto legacy=ViewportMaterial::create({1,1,1,1},chain.value()); CHECK(legacy);
  auto filtered=ViewportMaterial::create({1,1,1,1},chain.value(),TextureSampling::LinearClampMips); CHECK(filtered);
  CHECK(legacy.value()->sampling()==TextureSampling::LinearClampMip0);
  CHECK(filtered.value()->sampling()==TextureSampling::LinearClampMips);
  CHECK(legacy.value()->texture()==filtered.value()->texture());
  std::weak_ptr<const ViewportTexture> retained=chain.value(); chain.value().reset();
  legacy.value().reset(); CHECK(!retained.expired());
  CHECK(filtered.value()->texture()->mip(1)->pixels[0]==std::byte{128});
  filtered.value().reset(); CHECK(retained.expired());

  const std::array opaque{std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
  auto one=ViewportTexture::create(1,1,opaque),one_chain=ViewportTexture::create_mips(1,1,opaque);
  CHECK(one && one_chain && !one.value()->complete_mip_chain() && one_chain.value()->complete_mip_chain());
  CHECK(one.value()->mip_count()==1 && one_chain.value()->mip_count()==1);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},one.value(),TextureSampling::LinearClampMips),ErrorCode::InvalidInput);
  CHECK(ViewportMaterial::create({1,1,1,1},one_chain.value(),TextureSampling::LinearClampMips));
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},{},TextureSampling::LinearClampMips),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},one.value(),static_cast<TextureSampling>(0)),ErrorCode::IncompatibleVersion);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},one.value(),static_cast<TextureSampling>(3)),ErrorCode::IncompatibleVersion);
  CHECK_ERROR(ViewportTexture::create(3,5,texels),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_mips(3,5,std::span(texels).first(60)),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_mips(3,5,std::span(texels).first(71)),ErrorCode::InvalidInput);
  texels.push_back(std::byte{255}); CHECK_ERROR(ViewportTexture::create_mips(3,5,texels),ErrorCode::InvalidInput); texels.pop_back();
  texels[71]=std::byte{254}; CHECK_ERROR(ViewportTexture::create_mips(3,5,texels),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_mips(0,5,{}),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_mips(5,0,{}),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_mips(1025,1,{}),ErrorCode::Exhausted);
  CHECK_ERROR(ViewportTexture::create_mips(UINT32_MAX,UINT32_MAX,{}),ErrorCode::Exhausted);
  std::vector<std::byte> strip(32,std::byte{255});
  for (const auto dimensions:{std::array{1u,5u},std::array{5u,1u}}) {
    auto narrow=ViewportTexture::create_mips(dimensions[0],dimensions[1],strip); CHECK(narrow);
    CHECK(narrow.value()->mip_count()==3 && narrow.value()->pixels().size()==20);
    CHECK(narrow.value()->mip(2)->width==1 && narrow.value()->mip(2)->height==1);
  }

  // A complete maximum chain, unlike its base level, consumes 5,592,404 bytes.
  // Native frame admission separately exercises the actual six/seven boundary.
  std::vector<std::byte> maximum(5592404,std::byte{255});
  std::vector<std::shared_ptr<const ViewportTexture>> owned;
  std::size_t charged{};
  for (unsigned i=0;i<7;++i) {
    auto full=ViewportTexture::create_mips(1024,1024,maximum); CHECK(full);
    CHECK(full.value()->mip_count()==viewport_texture_max_mips && full.value()->pixels().size()==4194304);
    CHECK(full.value()->mip(10)->width==1 && full.value()->mip(10)->height==1);
    charged+=full.value()->texels().size(); owned.push_back(std::move(full).value());
    if (i==5) CHECK(charged==33554424 && charged<=viewport_texture_frame_bytes);
    if (i==6) CHECK(charged==39146828 && charged>viewport_texture_frame_bytes);
  }
}

void scalar_packet_contracts() {
  // Same packed NPOT layout, distinct immutable encoding and explicit role.
  std::vector<std::byte> bytes(72,std::byte{255}); bytes[0]=std::byte{128};
  auto scalar=ViewportTexture::create_scalar_mips(3,5,bytes); CHECK(scalar);
  auto color=ViewportTexture::create_mips(3,5,bytes); CHECK(color);
  CHECK(scalar.value()->encoding()==TextureEncoding::LinearData);
  CHECK(color.value()->encoding()==TextureEncoding::SrgbColor);
  CHECK(scalar.value()->complete_mip_chain() && scalar.value()->mip_count()==3);
  CHECK(scalar.value()->mip(1)->pixels.data()==scalar.value()->texels().data()+60);
  CHECK(scalar.value()->mip(2)->pixels.data()==scalar.value()->texels().data()+68);
  bytes[0]=std::byte{0}; CHECK(scalar.value()->pixels()[0]==std::byte{128});
  auto material=ViewportMaterial::create({.25f,.5f,1,1},scalar.value(),TextureSampling::LinearClampMips,
      TextureRole::DiffuseReflectanceR); CHECK(material);
  CHECK(material.value()->texture_role()==TextureRole::DiffuseReflectanceR);
  CHECK(material.value()->sampling()==TextureSampling::LinearClampMips);
  CHECK(material.value()->texture()==scalar.value());
  // An already retained/cached linear packet cannot be rebound as old color.
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},material.value()->texture()),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},scalar.value(),TextureSampling::LinearClampMips),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},scalar.value(),TextureSampling::LinearClampMip0,
      TextureRole::DiffuseReflectanceR),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},color.value(),TextureSampling::LinearClampMips,
      TextureRole::DiffuseReflectanceR),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportMaterial::create({1,1,1,1},{},TextureSampling::LinearClampMip0,
      TextureRole::DiffuseReflectanceR),ErrorCode::InvalidInput);
  for (const auto role:{static_cast<TextureRole>(0),static_cast<TextureRole>(3)})
    CHECK_ERROR(ViewportMaterial::create({1,1,1,1},color.value(),TextureSampling::LinearClampMips,role),ErrorCode::IncompatibleVersion);
  CHECK(ViewportMaterial::create().value()->texture_role()==TextureRole::Color);
  std::weak_ptr<const ViewportTexture> retained=scalar.value(); scalar.value().reset();
  CHECK(!retained.expired()); material.value().reset(); CHECK(retained.expired());
  const std::array one{std::byte{128},std::byte{0},std::byte{255},std::byte{255}};
  auto one_scalar=ViewportTexture::create_scalar_mips(1,1,one); CHECK(one_scalar);
  CHECK(one_scalar.value()->mip_count()==1 && one_scalar.value()->complete_mip_chain());
  CHECK(ViewportMaterial::create({1,1,1,1},one_scalar.value(),TextureSampling::LinearClampMips,TextureRole::DiffuseReflectanceR));
  CHECK_ERROR(ViewportTexture::create_scalar_mips(3,5,std::span(bytes).first(60)),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_scalar_mips(3,5,std::span(bytes).first(71)),ErrorCode::InvalidInput);
  bytes.push_back(std::byte{255}); CHECK_ERROR(ViewportTexture::create_scalar_mips(3,5,bytes),ErrorCode::InvalidInput); bytes.pop_back();
  for (const std::size_t alpha:{std::size_t{3},std::size_t{63},std::size_t{71}}) {
    bytes[alpha]=std::byte{254}; CHECK_ERROR(ViewportTexture::create_scalar_mips(3,5,bytes),ErrorCode::InvalidInput);
    bytes[alpha]=std::byte{255};
  }
  CHECK_ERROR(ViewportTexture::create_scalar_mips(0,1,{}),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_scalar_mips(1,0,{}),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create_scalar_mips(1025,1,{}),ErrorCode::Exhausted);
  CHECK_ERROR(ViewportTexture::create_scalar_mips(UINT32_MAX,UINT32_MAX,{}),ErrorCode::Exhausted);
}

int main() {
  mip_packet_contracts();
  scalar_packet_contracts();
  std::vector<std::byte> pixels(3*2*4,std::byte{255}); pixels[0]=std::byte{128};
  auto texture=ViewportTexture::create(3,2,pixels); CHECK(texture);
  CHECK(texture.value()->width()==3 && texture.value()->height()==2 && texture.value()->pixels().size()==24);
  pixels[0]=std::byte{0}; CHECK(texture.value()->pixels()[0]==std::byte{128});
  auto material=ViewportMaterial::create({.25f,.5f,1,1},texture.value()); CHECK(material);
  CHECK(material.value()->texture()==texture.value() && material.value()->base_color()[0]==.25f);
  std::weak_ptr<const ViewportTexture> retained=texture.value(); texture.value().reset();
  CHECK(!retained.expired() && material.value()->texture()->pixels()[0]==std::byte{128});
  material.value().reset(); CHECK(retained.expired());
  CHECK(ViewportMaterial::create());
  CHECK_ERROR(ViewportTexture::create(0,2,{}),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create(3,0,{}),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportTexture::create(1025,1,{}),ErrorCode::Exhausted);
  CHECK_ERROR(ViewportTexture::create(1,1025,{}),ErrorCode::Exhausted);
  CHECK_ERROR(ViewportTexture::create(UINT32_MAX,UINT32_MAX,{}),ErrorCode::Exhausted);
  CHECK_ERROR(ViewportTexture::create(3,2,std::span(pixels).first(23)),ErrorCode::InvalidInput);
  pixels.push_back(std::byte{255}); CHECK_ERROR(ViewportTexture::create(3,2,pixels),ErrorCode::InvalidInput); pixels.pop_back();
  pixels[7]=std::byte{254}; CHECK_ERROR(ViewportTexture::create(3,2,pixels),ErrorCode::InvalidInput);
  std::vector<std::byte> maximum(1024*1024*4,std::byte{255});
  CHECK(ViewportTexture::create(1024,1024,maximum));
  for (const float value:{-0.01f,1.01f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
    for (std::size_t component=0;component<4;++component) {
      std::array<float,4> color{1,1,1,1}; color[component]=value;
      CHECK_ERROR(ViewportMaterial::create(color),ErrorCode::InvalidInput);
    }
  CHECK_ERROR(ViewportMaterial::create({1,1,1,.5f}),ErrorCode::InvalidInput);
  CHECK(ViewportMaterial::create({0,0,0,1}));
  std::array<GeometryVertex,3> vertices{{{{0,0,0},{0,0,1},{0,1}},
      {{1,0,0},{0,0,1},{1,1}},{{0,1,0},{0,0,1},{0,0}}}};
  const std::array<std::uint32_t,3> indices{0,1,2};
  auto legacy=ViewportGeometry::create(vertices,indices); CHECK(legacy && !legacy.value()->has_uv());
  auto uv=ViewportGeometry::create(vertices,indices,true); CHECK(uv && uv.value()->has_uv());
  vertices[0].uv={1e12,-1e12}; CHECK(ViewportGeometry::create(vertices,indices,true));
  vertices[0].uv[0]=1e12+1; CHECK_ERROR(ViewportGeometry::create(vertices,indices,true),ErrorCode::InvalidInput);
  vertices[0].uv[0]=std::numeric_limits<double>::quiet_NaN();
  CHECK_ERROR(ViewportGeometry::create(vertices,indices,true),ErrorCode::InvalidInput);
  CHECK_ERROR(ViewportGeometry::create(vertices,indices),ErrorCode::InvalidInput);
  CHECK(uv.value()->vertices()[0].uv==std::array<double,2>{0,1});
  std::puts("viewport material/texture copied packet contracts passed");
}
