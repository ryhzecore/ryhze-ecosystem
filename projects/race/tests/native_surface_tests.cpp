#include "race/assets/surface.hpp"
#include "mesh_pipeline.hpp"
#include "test.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <new>

namespace {
thread_local std::size_t failure=SIZE_MAX;
void allocation_point(){if(failure!=SIZE_MAX){if(!failure){failure=SIZE_MAX;throw std::bad_alloc();}--failure;}}
}
// This executable's unaligned standard allocations exercise codec/importer
// Result boundaries. No production allocator or other executable is changed.
void* operator new(std::size_t bytes){allocation_point();if(auto p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* pointer) noexcept{std::free(pointer);}
void operator delete[](void* pointer) noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{std::free(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept{std::free(pointer);}

namespace {
using namespace race;
namespace s=assets::surface;
namespace m=assets::mesh;
namespace p=mesh_pipeline;
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
assets::Digest hash(std::string_view text){return assets::digest(bytes(text));}
s::Stamp stamp(){s::Stamp value;value.source=hash("surface-source");value.settings=hash("surface-settings");value.importer=hash("surface-importer");return value;}
s::Stamp cooked(){auto value=stamp();value.representation=s::Representation::Cooked;value.target=1;value.cooker=hash("surface-cooker");return value;}
constexpr std::array<std::byte,4> pixel{std::byte{20},std::byte{40},std::byte{80},std::byte{255}};
void put(std::vector<std::byte>& data,std::size_t at,std::uint64_t value,std::size_t width=8){
  for(std::size_t i=0;i<width;++i)data[at+i]=std::byte((value>>(i*8))&255);
}
void resign(std::vector<std::byte>& data){const auto checksum=assets::digest(std::span<const std::byte>(data).first(data.size()-32));std::copy(checksum.begin(),checksum.end(),data.end()-32);}
void texture_wire(){
  auto encoded=s::encode_texture({7,9},stamp(),1,1,s::Encoding::SrgbColor,pixel);CHECK(encoded);
  const auto& product=encoded.value();CHECK(product.size()==292);
  // Independent Python struct.pack/hashlib vectors, not codec round trips.
  CHECK(assets::hex(assets::digest(product))=="a5d0f5ca27b936a6fb054edf969cd07f57ee9831ae70b494c1bae53748f25a15");
  auto view=s::validate_texture(product);CHECK(view);CHECK(view.value().id()==PersistentId{7,9});
  CHECK(view.value().stamp()==stamp()&&view.value().revision()==assets::digest(product));
  CHECK(view.value().width()==1&&view.value().height()==1&&view.value().encoding()==s::Encoding::SrgbColor);
  CHECK(view.value().schema()==1&&view.value().mip_count()==1&&view.value().texels().size()==4);
  CHECK(view.value().mip(0)&&!view.value().mip(1));
  CHECK(std::equal(pixel.begin(),pixel.end(),view.value().pixels().begin(),view.value().pixels().end()));
  CHECK(s::encode_texture({7,9},stamp(),1,1,s::Encoding::SrgbColor,pixel).value()==product);
  failure=0;auto no_allocation=s::validate_texture(product);CHECK(failure==0);failure=SIZE_MAX;CHECK(no_allocation);
  for(std::size_t n=0;n<product.size();++n)CHECK(!s::validate_texture(std::span(product).first(n)));
  for(const auto at:{0U,8U,12U,16U,20U,24U,28U,240U,244U,248U,252U}){
    auto invalid=product;put(invalid,at,UINT32_MAX,4);resign(invalid);CHECK(!s::validate_texture(invalid));
  }
  for(const auto at:{32U,40U,48U,56U}){auto invalid=product;put(invalid,at,UINT64_MAX);resign(invalid);CHECK(!s::validate_texture(invalid));}
  auto invalid=product;put(invalid,64,0);put(invalid,72,0);resign(invalid);CHECK(!s::validate_texture(invalid));
  invalid=product;invalid[256]^=std::byte{1};CHECK(!s::validate_texture(invalid));
  invalid=product;invalid.push_back(std::byte{});CHECK(!s::validate_texture(invalid));
  auto bad_stamp=stamp();bad_stamp.dependencies=hash("wrong dependency");CHECK(!s::encode_texture({7,9},bad_stamp,1,1,s::Encoding::SrgbColor,pixel));
  CHECK(!s::encode_texture({},stamp(),1,1,s::Encoding::SrgbColor,pixel));
  CHECK(!s::encode_texture({7,9},stamp(),0,1,s::Encoding::SrgbColor,pixel));
  CHECK(!s::encode_texture({7,9},stamp(),1025,1,s::Encoding::SrgbColor,pixel));
  CHECK(!s::encode_texture({7,9},stamp(),1,1,static_cast<s::Encoding>(3),pixel));
  CHECK(!s::encode_texture({7,9},stamp(),1,1,s::Encoding::SrgbColor,std::span(pixel).first(3)));
  s::Limits limits;limits.product_bytes=product.size();CHECK(s::validate_texture(product,limits));
  --limits.product_bytes;CHECK(!s::validate_texture(product,limits));limits={};limits.dimension=SIZE_MAX;CHECK(!s::validate_texture(product,limits));
  std::vector<std::byte> maximum(1024U*1024U*4U,std::byte{255});
  auto max_product=s::encode_texture({7,9},cooked(),1024,1024,s::Encoding::SrgbColor,maximum);CHECK(max_product&&s::validate_texture(max_product.value()));
}
void texture_mip_wire(){
  auto one=s::encode_texture_mips({7,9},cooked(),1,1,s::Encoding::SrgbColor,pixel);CHECK(one);
  // Independent struct.pack/hashlib schema-2 vector, including explicit 1x1.
  CHECK(assets::hex(assets::digest(one.value()))=="7d3cd69efdf5bda1c24dfa710853a995cf8bd61295e34de791c25ba30837b103");
  auto one_view=s::validate_texture(one.value());CHECK(one_view&&one_view.value().schema()==2&&one_view.value().mip_count()==1);
  CHECK(one.value()!=s::encode_texture({7,9},cooked(),1,1,s::Encoding::SrgbColor,pixel).value());
  std::vector<std::byte> chain(72,std::byte{255}); // 3x5, 1x2, 1x1.
  auto encoded=s::encode_texture_mips({7,9},cooked(),3,5,s::Encoding::SrgbColor,chain);CHECK(encoded);
  const auto& product=encoded.value();auto view=s::validate_texture(product);CHECK(view);
  CHECK(view.value().schema()==2&&view.value().mip_count()==3&&view.value().pixels().size()==60&&view.value().texels().size()==72);
  CHECK(view.value().pixels().data()==view.value().texels().data());
  CHECK(view.value().mip(0)->width==3&&view.value().mip(0)->height==5&&view.value().mip(0)->pixels.size()==60);
  CHECK(view.value().mip(1)->width==1&&view.value().mip(1)->height==2&&view.value().mip(1)->pixels.size()==8);
  CHECK(view.value().mip(2)->width==1&&view.value().mip(2)->height==1&&view.value().mip(2)->pixels.size()==4);
  CHECK(view.value().mip(1)->pixels.data()==view.value().texels().data()+60);
  CHECK(view.value().mip(2)->pixels.data()==view.value().texels().data()+68);
  CHECK(!view.value().mip(3)&&!view.value().mip(SIZE_MAX));
  failure=0;auto no_allocation=s::validate_texture(product);const auto tail=view.value().mip(2);CHECK(failure==0);failure=SIZE_MAX;CHECK(no_allocation&&tail);
  for(const auto count:{0U,1U,2U,4U,12U,UINT32_MAX}){
    auto invalid=product;put(invalid,252,count,4);resign(invalid);CHECK(!s::validate_texture(invalid));
  }
  for(const auto schema:{0U,1U,3U,UINT32_MAX}){
    auto invalid=product;put(invalid,20,schema,4);resign(invalid);CHECK(!s::validate_texture(invalid));
  }
  auto invalid=product;put(invalid,56,1);resign(invalid);CHECK(!s::validate_texture(invalid));
  invalid=product;put(invalid,248,2,4);resign(invalid);CHECK(!s::validate_texture(invalid));
  invalid=product;put(invalid,24,1,4);put(invalid,28,0,4);std::fill(invalid.begin()+176,invalid.begin()+208,std::byte{});resign(invalid);
  CHECK(!s::validate_texture(invalid)); // Otherwise valid Imported stamp cannot carry cooked schema 2.
  invalid=product;invalid[s::header_bytes+60+3]=std::byte{254};resign(invalid);
  CHECK_ERROR(s::validate_texture(invalid),ErrorCode::Unsupported); // Non-base alpha must be validated.
  invalid=product;invalid[s::header_bytes+3]=std::byte{};resign(invalid);CHECK(!s::validate_texture(invalid));
  invalid=product;invalid.back()^=std::byte{1};CHECK(!s::validate_texture(invalid));
  for(const bool extra:{false,true}){
    invalid=product;
    if(extra)invalid.insert(invalid.end()-32,4,std::byte{255});
    else invalid.erase(invalid.end()-36,invalid.end()-32);
    put(invalid,32,invalid.size());put(invalid,48,invalid.size()-s::header_bytes-s::checksum_bytes);resign(invalid);
    CHECK(!s::validate_texture(invalid));
  }
  CHECK(!s::encode_texture_mips({7,9},stamp(),3,5,s::Encoding::SrgbColor,chain));
  CHECK(!s::encode_texture_mips({7,9},cooked(),3,5,s::Encoding::LinearData,chain));
  CHECK(!s::encode_texture_mips({7,9},cooked(),3,5,s::Encoding::SrgbColor,std::span(chain).first(68)));
  chain[63]=std::byte{254};CHECK_ERROR(s::encode_texture_mips({7,9},cooked(),3,5,s::Encoding::SrgbColor,chain),ErrorCode::Unsupported);chain[63]=std::byte{255};
  s::Limits limits;limits.product_bytes=product.size();CHECK(s::validate_texture(product,limits));
  --limits.product_bytes;
  const auto provenance=cooked();
  failure=0;auto limited=s::encode_texture_mips({7,9},provenance,3,5,s::Encoding::SrgbColor,chain,limits);CHECK(failure==0);failure=SIZE_MAX;
  CHECK_ERROR(limited,ErrorCode::Exhausted);CHECK_ERROR(s::validate_texture(product,limits),ErrorCode::Exhausted);
  failure=0;auto overflow=s::encode_texture_mips({7,9},provenance,UINT32_MAX,UINT32_MAX,s::Encoding::SrgbColor,chain);CHECK(failure==0);failure=SIZE_MAX;
  CHECK_ERROR(overflow,ErrorCode::Exhausted);
  for(const auto dimensions:std::array<std::array<std::uint32_t,2>,2>{{{1,8},{8,1}}}){
    std::vector<std::byte> line(60,std::byte{255}); // 8+4+2+1 pixels.
    auto value=s::encode_texture_mips({7,9},provenance,dimensions[0],dimensions[1],s::Encoding::SrgbColor,line);CHECK(value);
    auto decoded=s::validate_texture(value.value());CHECK(decoded&&decoded.value().mip_count()==4);
    CHECK(decoded.value().mip(3)->width==1&&decoded.value().mip(3)->height==1);
  }
  std::vector<std::byte> maximum(5592404,std::byte{255});
  auto maximum_product=s::encode_texture_mips({7,9},provenance,1024,1024,s::Encoding::SrgbColor,maximum);CHECK(maximum_product);
  CHECK(maximum_product.value().size()==5592692);
  auto maximum_view=s::validate_texture(maximum_product.value());CHECK(maximum_view&&maximum_view.value().mip_count()==11);
  CHECK(maximum_view.value().pixels().size()==4194304&&maximum_view.value().texels().size()==5592404);
  CHECK(maximum_view.value().mip(10)->pixels.size()==4&&!maximum_view.value().mip(11));
  failure=0;auto allocation=s::encode_texture_mips({7,9},provenance,3,5,s::Encoding::SrgbColor,chain);failure=SIZE_MAX;CHECK_ERROR(allocation,ErrorCode::Exhausted);
  CHECK(s::encode_texture_mips({7,9},provenance,3,5,s::Encoding::SrgbColor,chain));
}
void texture_scalar_mip_wire(){
  const auto provenance=cooked();
  auto one=s::encode_texture_scalar_mips({7,9},provenance,1,1,s::Encoding::LinearData,pixel);CHECK(one);
  // Independent Python struct.pack/hashlib vector: schema 3, LinearData, one explicit complete level.
  CHECK(assets::hex(assets::digest(one.value()))=="4b9d71c56df371f2b3f09c4ac9401e122355793b698a3cde22e7180071cb5be0");
  auto one_view=s::validate_texture(one.value());CHECK(one_view&&one_view.value().schema()==3&&one_view.value().mip_count()==1);
  CHECK(one.value()!=s::encode_texture({7,9},provenance,1,1,s::Encoding::LinearData,pixel).value());
  std::vector<std::byte> chain(72,std::byte{255}); // 3x5, 1x2, 1x1.
  auto encoded=s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::LinearData,chain);CHECK(encoded);
  const auto& product=encoded.value();auto view=s::validate_texture(product);CHECK(view);
  CHECK(view.value().encoding()==s::Encoding::LinearData&&view.value().schema()==3&&view.value().mip_count()==3);
  CHECK(view.value().pixels().size()==60&&view.value().texels().size()==72&&view.value().mip(1)->pixels.data()==view.value().texels().data()+60);
  CHECK(view.value().mip(1)->width==1&&view.value().mip(1)->height==2&&view.value().mip(2)->pixels.size()==4);
  CHECK(!view.value().mip(3)&&!view.value().mip(SIZE_MAX));
  failure=0;auto admitted=s::validate_texture(product);auto tail=view.value().mip(2);CHECK(failure==0);failure=SIZE_MAX;CHECK(admitted&&tail);
  for(std::size_t n=0;n<product.size();++n)CHECK(!s::validate_texture(std::span(product).first(n)));
  for(const auto schema:{0U,1U,2U,4U,UINT32_MAX}){
    auto invalid=product;put(invalid,20,schema,4);resign(invalid);CHECK(!s::validate_texture(invalid));
  }
  for(const auto count:{0U,1U,2U,4U,12U,UINT32_MAX}){
    auto invalid=product;put(invalid,252,count,4);resign(invalid);CHECK(!s::validate_texture(invalid));
  }
  auto invalid=product;put(invalid,248,1,4);resign(invalid);CHECK_ERROR(s::validate_texture(invalid),ErrorCode::IncompatibleVersion);
  invalid=product;put(invalid,24,1,4);put(invalid,28,0,4);std::fill(invalid.begin()+176,invalid.begin()+208,std::byte{});resign(invalid);
  CHECK_ERROR(s::validate_texture(invalid),ErrorCode::IncompatibleVersion);
  invalid=product;put(invalid,56,1);resign(invalid);CHECK(!s::validate_texture(invalid));
  invalid=product;put(invalid,16,3,4);resign(invalid);CHECK_ERROR(s::validate_material(invalid),ErrorCode::IncompatibleVersion);
  for(const auto at:{s::header_bytes+3,s::header_bytes+63,s::header_bytes+71}){
    invalid=product;invalid[at]=std::byte{254};resign(invalid);CHECK_ERROR(s::validate_texture(invalid),ErrorCode::Unsupported);
  }
  invalid=product;invalid.back()^=std::byte{1};CHECK(!s::validate_texture(invalid));
  for(const bool extra:{false,true}){
    invalid=product;if(extra)invalid.insert(invalid.end()-32,4,std::byte{255});else invalid.erase(invalid.end()-36,invalid.end()-32);
    put(invalid,32,invalid.size());put(invalid,48,invalid.size()-s::header_bytes-s::checksum_bytes);resign(invalid);CHECK(!s::validate_texture(invalid));
  }
  CHECK(!s::encode_texture_scalar_mips({7,9},stamp(),3,5,s::Encoding::LinearData,chain));
  CHECK(!s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::SrgbColor,chain));
  CHECK(!s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::LinearData,std::span(chain).first(68)));
  chain[63]=std::byte{254};CHECK_ERROR(s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::LinearData,chain),ErrorCode::Unsupported);chain[63]=std::byte{255};
  s::Limits limits;limits.product_bytes=product.size();CHECK(s::validate_texture(product,limits));--limits.product_bytes;
  failure=0;auto limited=s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::LinearData,chain,limits);CHECK(failure==0);failure=SIZE_MAX;
  CHECK_ERROR(limited,ErrorCode::Exhausted);CHECK_ERROR(s::validate_texture(product,limits),ErrorCode::Exhausted);
  failure=0;auto overflow=s::encode_texture_scalar_mips({7,9},provenance,UINT32_MAX,UINT32_MAX,s::Encoding::LinearData,chain);CHECK(failure==0);failure=SIZE_MAX;
  CHECK_ERROR(overflow,ErrorCode::Exhausted);
  for(const auto dimensions:std::array<std::array<std::uint32_t,2>,2>{{{1,8},{8,1}}}){
    std::vector<std::byte> line(60,std::byte{255});auto value=s::encode_texture_scalar_mips({7,9},provenance,dimensions[0],dimensions[1],s::Encoding::LinearData,line);CHECK(value);
    auto decoded=s::validate_texture(value.value());CHECK(decoded&&decoded.value().mip_count()==4&&decoded.value().mip(3)->width==1&&decoded.value().mip(3)->height==1);
  }
  std::vector<std::byte> maximum(5592404,std::byte{255});
  auto max_product=s::encode_texture_scalar_mips({7,9},provenance,1024,1024,s::Encoding::LinearData,maximum);CHECK(max_product&&max_product.value().size()==5592692);
  auto max_view=s::validate_texture(max_product.value());CHECK(max_view&&max_view.value().mip_count()==11&&max_view.value().mip(10)->pixels.size()==4);
  failure=0;auto allocation=s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::LinearData,chain);failure=SIZE_MAX;CHECK_ERROR(allocation,ErrorCode::Exhausted);
  CHECK(s::encode_texture_scalar_mips({7,9},provenance,3,5,s::Encoding::LinearData,chain));
  // Exact scalar pins remain incompatible with both existing diffuse samplers.
  for(const auto sampling:{s::Sampling::LinearClampMip0,s::Sampling::LinearClampMips}){
    s::Material material;material.sampling=sampling;material.base_color_texture=s::Reference{one_view.value().id(),one_view.value().revision()};
    auto material_stamp=provenance;material_stamp.dependencies=s::dependency_digest(material.base_color_texture);
    auto bound=s::encode_material({8,9},material_stamp,material);CHECK(bound);auto decoded=s::validate_material(bound.value());CHECK(decoded);
    CHECK_ERROR(s::validate_binding(decoded.value(),one_view.value()),ErrorCode::IncompatibleVersion);
    auto unsupported=bound.value();put(unsupported,20,3,4);put(unsupported,248,3,4);resign(unsupported);
    CHECK_ERROR(s::validate_material(unsupported),ErrorCode::IncompatibleVersion);
  }
}
void material_mip_binding(){
  const auto provenance=cooked();
  auto texture=s::encode_texture_mips({0,10},provenance,1,1,s::Encoding::SrgbColor,pixel);CHECK(texture);
  auto view=s::validate_texture(texture.value());CHECK(view);
  s::Material material;material.sampling=s::Sampling::LinearClampMips;
  material.base_color_texture=s::Reference{view.value().id(),view.value().revision()};
  auto material_stamp=stamp();material_stamp.dependencies=s::dependency_digest(material.base_color_texture);
  auto product=s::encode_material({7,9},material_stamp,material);CHECK(product);
  auto decoded=s::validate_material(product.value());CHECK(decoded&&decoded.value().schema()==2&&decoded.value().material()==material);
  CHECK(s::validate_binding(decoded.value(),view.value()));
  failure=0;auto no_allocation=s::validate_material(product.value());auto binding=s::validate_binding(decoded.value(),view.value());CHECK(failure==0);failure=SIZE_MAX;CHECK(no_allocation&&binding);
  auto legacy=material;legacy.sampling=s::Sampling::LinearClampMip0;
  auto legacy_material=s::encode_material({7,9},material_stamp,legacy);CHECK(legacy_material);
  CHECK(s::validate_material(legacy_material.value()).value().schema()==1);
  CHECK(s::validate_binding(s::validate_material(legacy_material.value()).value(),view.value())); // Mip0 may pin a full chain.
  auto old_texture=s::encode_texture({0,10},provenance,1,1,s::Encoding::SrgbColor,pixel);CHECK(old_texture);
  auto old_view=s::validate_texture(old_texture.value());CHECK(old_view);
  auto wrong_schema=material;wrong_schema.base_color_texture=s::Reference{old_view.value().id(),old_view.value().revision()};
  auto wrong_stamp=material_stamp;wrong_stamp.dependencies=s::dependency_digest(wrong_schema.base_color_texture);
  auto wrong_product=s::encode_material({7,9},wrong_stamp,wrong_schema);CHECK(wrong_product);
  CHECK_ERROR(s::validate_binding(s::validate_material(wrong_product.value()).value(),old_view.value()),ErrorCode::IncompatibleVersion);
  auto wrong_pin=material;wrong_pin.base_color_texture->asset.low=11;wrong_stamp.dependencies=s::dependency_digest(wrong_pin.base_color_texture);
  wrong_product=s::encode_material({7,9},wrong_stamp,wrong_pin);CHECK(wrong_product);
  CHECK_ERROR(s::validate_binding(s::validate_material(wrong_product.value()).value(),view.value()),ErrorCode::InvalidDependency);
  for(const auto at:{20U,248U}){
    auto invalid=product.value();put(invalid,at,1,4);resign(invalid);CHECK(!s::validate_material(invalid));
  }
  auto missing=material;missing.base_color_texture.reset();auto empty=stamp();
  CHECK_ERROR(s::encode_material({7,9},empty,missing),ErrorCode::InvalidDependency);
  auto unknown=material;unknown.sampling=static_cast<s::Sampling>(3);CHECK(!s::encode_material({7,9},material_stamp,unknown));
  failure=0;auto allocation=s::encode_material({7,9},material_stamp,material);failure=SIZE_MAX;CHECK_ERROR(allocation,ErrorCode::Exhausted);
  CHECK(s::encode_material({7,9},material_stamp,material));
}
void material_scalar_binding(){
  const auto provenance=cooked();
  auto texture=s::encode_texture_scalar_mips({0,10},provenance,1,1,s::Encoding::LinearData,pixel);CHECK(texture);
  auto view=s::validate_texture(texture.value());CHECK(view);
  s::Material material;material.base_color={.25,.5,1};material.sampling=s::Sampling::LinearClampMips;
  material.texture_role=s::TextureRole::DiffuseReflectanceR;
  material.base_color_texture=s::Reference{view.value().id(),view.value().revision()};
  auto material_stamp=stamp();material_stamp.dependencies=s::dependency_digest(material.base_color_texture);
  auto product=s::encode_material({7,9},material_stamp,material);CHECK(product);
  // Independent Python struct.pack/hashlib vector; schema3 and role2, sampler remains2.
  CHECK(assets::hex(assets::digest(product.value()))=="a60146d1e02526c715603e5629b1e69a4915fb83a0e0f3114496bb3a0aa8c554");
  auto decoded=s::validate_material(product.value());CHECK(decoded&&decoded.value().schema()==3&&decoded.value().material()==material);
  CHECK(s::validate_binding(decoded.value(),view.value()));
  failure=0;auto borrowed=s::validate_material(product.value());auto binding=s::validate_binding(decoded.value(),view.value());
  CHECK(failure==0);failure=SIZE_MAX;CHECK(borrowed&&binding);
  for(std::size_t n=0;n<product.value().size();++n)CHECK(!s::validate_material(std::span(product.value()).first(n)));
  for(const auto role:{0U,1U,3U,0x10000002U,UINT32_MAX}){
    auto invalid=product.value();put(invalid,252,role,4);resign(invalid);CHECK_ERROR(s::validate_material(invalid),ErrorCode::IncompatibleVersion);
  }
  for(const auto schema:{1U,2U,4U}){
    auto invalid=product.value();put(invalid,20,schema,4);resign(invalid);CHECK_ERROR(s::validate_material(invalid),ErrorCode::IncompatibleVersion);
  }
  for(const auto sampler:{0U,1U,3U}){
    auto invalid=product.value();put(invalid,248,sampler,4);resign(invalid);CHECK_ERROR(s::validate_material(invalid),ErrorCode::IncompatibleVersion);
  }
  const std::array incompatible{
    s::encode_texture({0,10},provenance,1,1,s::Encoding::SrgbColor,pixel).value(),
    s::encode_texture_mips({0,10},provenance,1,1,s::Encoding::SrgbColor,pixel).value(),
    s::encode_texture({0,10},provenance,1,1,s::Encoding::LinearData,pixel).value(),
    s::encode_texture({0,10},stamp(),1,1,s::Encoding::LinearData,pixel).value()};
  for(const auto& bytes:incompatible){
    auto other=s::validate_texture(bytes);CHECK(other);auto candidate=material;
    candidate.base_color_texture=s::Reference{other.value().id(),other.value().revision()};
    auto pinned=material_stamp;pinned.dependencies=s::dependency_digest(candidate.base_color_texture);
    auto wire=s::encode_material({7,9},pinned,candidate);CHECK(wire);
    CHECK_ERROR(s::validate_binding(s::validate_material(wire.value()).value(),other.value()),ErrorCode::IncompatibleVersion);
  }
  auto wrong=material;wrong.base_color_texture->revision[0]^=std::byte{1};auto changed_stamp=material_stamp;
  changed_stamp.dependencies=s::dependency_digest(wrong.base_color_texture);
  auto changed=s::encode_material({7,9},changed_stamp,wrong);CHECK(changed);
  CHECK_ERROR(s::validate_binding(s::validate_material(changed.value()).value(),view.value()),ErrorCode::InvalidDependency);
  for(const auto sampling:{s::Sampling::LinearClampMip0,s::Sampling::LinearClampMips}){
    auto color=material;color.texture_role=s::TextureRole::Color;color.sampling=sampling;
    auto old=s::encode_material({7,9},material_stamp,color);CHECK(old);
    CHECK(s::validate_material(old.value()).value().material().texture_role==s::TextureRole::Color);
    CHECK_ERROR(s::validate_binding(s::validate_material(old.value()).value(),view.value()),ErrorCode::IncompatibleVersion);
    auto nonzero_reserved=old.value();put(nonzero_reserved,252,1,4);resign(nonzero_reserved);CHECK(!s::validate_material(nonzero_reserved));
  }
  auto missing=material;missing.base_color_texture.reset();CHECK_ERROR(s::encode_material({7,9},stamp(),missing),ErrorCode::InvalidDependency);
  auto mip0=material;mip0.sampling=s::Sampling::LinearClampMip0;CHECK_ERROR(s::encode_material({7,9},material_stamp,mip0),ErrorCode::IncompatibleVersion);
  auto unknown=material;unknown.texture_role=static_cast<s::TextureRole>(3);CHECK_ERROR(s::encode_material({7,9},material_stamp,unknown),ErrorCode::IncompatibleVersion);
  auto alpha=texture.value();alpha[s::header_bytes+3]=std::byte{254};resign(alpha);CHECK_ERROR(s::validate_texture(alpha),ErrorCode::Unsupported);
  failure=0;auto allocation=s::encode_material({7,9},material_stamp,material);failure=SIZE_MAX;CHECK_ERROR(allocation,ErrorCode::Exhausted);
  CHECK(s::encode_material({7,9},material_stamp,material).value()==product.value());
}
void material_wire_and_binding(){
  s::Material plain;plain.base_color={.25,.5,1};
  auto encoded=s::encode_material({7,9},stamp(),plain);CHECK(encoded);const auto& product=encoded.value();CHECK(product.size()==360);
  CHECK(assets::hex(assets::digest(product))=="ec808dc5a56ae8c1c4de48293428305a26edc4ae885b263373e514e547850810");
  auto view=s::validate_material(product);CHECK(view);CHECK(view.value().material()==plain&&view.value().stamp()==stamp());
  CHECK(view.value().id()==PersistentId{7,9}&&view.value().revision()==assets::digest(product));
  failure=0;auto no_allocation=s::validate_material(product);CHECK(failure==0);failure=SIZE_MAX;CHECK(no_allocation);
  for(std::size_t n=0;n<product.size();++n)CHECK(!s::validate_material(std::span(product).first(n)));
  for(const auto at:{8U,12U,16U,20U,24U,28U,240U,244U,248U,252U}){
    auto invalid=product;put(invalid,at,UINT32_MAX,4);resign(invalid);CHECK(!s::validate_material(invalid));
  }
  for(const auto at:{32U,40U,48U,56U}){auto invalid=product;put(invalid,at,UINT64_MAX);resign(invalid);CHECK(!s::validate_material(invalid));}
  auto invalid=product;put(invalid,280,1);resign(invalid);CHECK(!s::validate_material(invalid)); // Unmarked optional pin.
  for(const auto value:{-0.1,1.1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    invalid=product;put(invalid,256,std::bit_cast<std::uint64_t>(value));resign(invalid);CHECK(!s::validate_material(invalid));
  }
  auto precise=plain;precise.base_color[0]=0x1.0000000000001p-2;
  auto exact=s::encode_material({7,9},stamp(),precise);CHECK(exact&&s::validate_material(exact.value()).value().material()==precise);
  auto texture=s::encode_texture({0,10},cooked(),1,1,s::Encoding::SrgbColor,pixel);CHECK(texture);
  auto texture_view=s::validate_texture(texture.value());CHECK(texture_view);
  s::Material material;material.base_color_texture=s::Reference{texture_view.value().id(),texture_view.value().revision()};
  auto provenance=stamp();provenance.dependencies=s::dependency_digest(material.base_color_texture);
  auto imported=s::encode_material({7,9},provenance,material);CHECK(imported);
  auto material_view=s::validate_material(imported.value());CHECK(material_view&&s::validate_binding(material_view.value(),texture_view.value()));
  CHECK(provenance.dependencies!=assets::digest({}));
  CHECK(!s::encode_material({7,9},stamp(),material));
  auto wrong=material;wrong.base_color_texture->revision.back()^=std::byte{1};auto wrong_stamp=provenance;wrong_stamp.dependencies=s::dependency_digest(wrong.base_color_texture);
  auto wrong_product=s::encode_material({7,9},wrong_stamp,wrong);CHECK(wrong_product);
  CHECK_ERROR(s::validate_binding(s::validate_material(wrong_product.value()).value(),texture_view.value()),ErrorCode::InvalidDependency);
  auto linear=s::encode_texture({0,10},cooked(),1,1,s::Encoding::LinearData,pixel);CHECK(linear);
  auto alpha=pixel;alpha[3]=std::byte{254};auto translucent=s::encode_texture({0,10},cooked(),1,1,s::Encoding::SrgbColor,alpha);CHECK(translucent);
  auto not_cooked=s::encode_texture({0,10},stamp(),1,1,s::Encoding::SrgbColor,pixel);CHECK(not_cooked);
  for(const auto& bytes_to_bind:{linear.value(),translucent.value(),not_cooked.value()}){
    auto texture_candidate=s::validate_texture(bytes_to_bind);CHECK(texture_candidate);
    auto candidate=material;candidate.base_color_texture=s::Reference{texture_candidate.value().id(),texture_candidate.value().revision()};
    auto candidate_stamp=provenance;candidate_stamp.dependencies=s::dependency_digest(candidate.base_color_texture);
    auto candidate_product=s::encode_material({7,9},candidate_stamp,candidate);CHECK(candidate_product);
    CHECK(!s::validate_binding(s::validate_material(candidate_product.value()).value(),texture_candidate.value()));
  }
  auto absent=material;absent.base_color_texture=s::Reference{{1,0},{}};CHECK(!s::encode_material({7,9},provenance,absent));
  plain.model=static_cast<s::Model>(2);CHECK(!s::encode_material({7,9},stamp(),plain));
}
constexpr std::string_view positions="v 0 0 0\nv 1 0 0\nv 0 1 0\n";
constexpr std::string_view mapped="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
void mesh_uv(){
  const std::array<m::Vertex,3> vertices{{{{0,0,0},{0,0,1},{0,1}},{{1,0,0},{0,0,1},{1,1}},{{0,1,0},{0,0,1},{0,0}}}};
  const std::array<std::uint32_t,3> indices{0,1,2};
  m::Stamp provenance;provenance.source=hash("mesh source v1");provenance.settings=hash("mesh settings v1");provenance.importer=hash("mesh importer v1");
  auto old=m::encode({1,2},provenance,vertices,indices);CHECK(old&&old.value().size()==444);
  CHECK(assets::hex(assets::digest(old.value()))=="ffc311cbef778fbd8bbcfe5f92d1dcc7bc027edff590ea607d84e5b98b05eda4");
  CHECK(!m::validate(old.value()).value().has_uv()&&m::validate(old.value()).value().vertex(0).value().uv==std::array<double,2>{});
  auto uv=m::encode({1,2},provenance,vertices,indices,{},true);CHECK(uv&&uv.value().size()==492);
  auto decoded=m::validate(uv.value());CHECK(decoded&&decoded.value().has_uv());
  for(std::size_t i=0;i<3;++i)CHECK(decoded.value().vertex(i).value()==vertices[i]&&decoded.value().index(i).value()==indices[i]);
  auto invalid=uv.value();put(invalid,20,1,4);resign(invalid);CHECK(!m::validate(invalid));
  invalid=uv.value();put(invalid,240,48,4);resign(invalid);CHECK(!m::validate(invalid));
  for(const auto value:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),1e13}){
    invalid=uv.value();put(invalid,304,std::bit_cast<std::uint64_t>(value));resign(invalid);CHECK(!m::validate(invalid));
  }
  auto imported=p::import_obj({11,12},bytes(mapped));CHECK(imported);auto imported_view=m::validate(imported.value());CHECK(imported_view&&imported_view.value().has_uv());
  CHECK(imported_view.value().vertex(0).value().uv==std::array<double,2>{0,1});
  CHECK(imported_view.value().vertex(2).value().uv==std::array<double,2>{0,0});
  CHECK(imported_view.value().stamp().settings!=p::import_stamp(assets::digest(bytes(mapped))).settings);
  CHECK(imported_view.value().stamp().importer!=p::import_stamp(assets::digest(bytes(mapped))).importer);
  auto cooked_mesh=p::cook(imported.value());CHECK(cooked_mesh&&m::validate(cooked_mesh.value()).value().has_uv());
  CHECK(m::validate(cooked_mesh.value()).value().vertex(0).value().uv==std::array<double,2>{0,1});
  CHECK(p::cook(imported.value()).value()==cooked_mesh.value());
  CHECK(p::import_obj({11,12},bytes(mapped)).value()==imported.value());
  for(const auto suffix:{"vt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2 3/3","vt 0 0\nf 1/1 2/2 3/3",
      "vt 0 0\nvt 1 0\nvt 0 1\nf 1//1 2//2 3//3","vt 0 0\nvt 1 0\nvt 0 1\nf 1/1/1 2/2/1 3/3/1",
      "vt 0 0 0\nf 1 2 3","vt nan 0\nf 1 2 3","vt 0 0\nf 1 2 3",
      "vt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\nf 1 2 3","f 1 2 3\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3"}){
    auto source=std::string(positions)+suffix;CHECK(!p::import_obj({11,12},bytes(source)));
  }
  p::Limits limits;limits.mesh.product_bytes=491;CHECK_ERROR(p::import_obj({11,12},bytes(mapped),limits),ErrorCode::Exhausted);
  limits.mesh.product_bytes=492;CHECK(p::import_obj({11,12},bytes(mapped),limits));
}
void allocations(){
  auto surface_stamp=stamp();s::Material material;
  failure=0;auto texture=s::encode_texture({7,9},surface_stamp,1,1,s::Encoding::SrgbColor,pixel);failure=SIZE_MAX;CHECK_ERROR(texture,ErrorCode::Exhausted);
  CHECK(s::encode_texture({7,9},surface_stamp,1,1,s::Encoding::SrgbColor,pixel));
  failure=0;auto value=s::encode_material({7,9},surface_stamp,material);failure=SIZE_MAX;CHECK_ERROR(value,ErrorCode::Exhausted);
  CHECK(s::encode_material({7,9},surface_stamp,material));
  std::size_t failures{};bool recovered=false;
  for(std::size_t step=0;step<16&&!recovered;++step){
    failure=step;auto result=p::import_obj({11,12},bytes(mapped));failure=SIZE_MAX;
    if(result)recovered=true;else{CHECK_ERROR(result,ErrorCode::Exhausted);++failures;CHECK(p::import_obj({11,12},bytes(mapped)));}
  }
  CHECK(recovered&&failures>=4);
}
} // namespace
int main(){texture_wire();texture_mip_wire();texture_scalar_mip_wire();material_wire_and_binding();material_mip_binding();material_scalar_binding();mesh_uv();allocations();std::puts("native surface codecs, exact pins, UV schema compatibility and allocation recovery passed");}
