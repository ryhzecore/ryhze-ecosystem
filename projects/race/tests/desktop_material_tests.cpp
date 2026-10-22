#include "desktop_package.hpp"
#include "mesh_pipeline.hpp"
#include "assets_support.hpp"
#include <algorithm>

namespace {
namespace d=race::desktop;
namespace sf=race::assets::surface;
namespace pak=race::assets::pak;
namespace m=race::assets::mesh;
sf::Stamp stamp(std::string_view label="surface-test"){
  sf::Stamp out;out.representation=sf::Representation::Cooked;out.target=sf::portable_cpu_target;
  out.source=out.settings=out.importer=out.cooker=assets::digest(std::as_bytes(std::span(label)));return out;
}
std::vector<std::byte> texture(AssetId id={92,1},std::uint32_t dimension=2,sf::Encoding encoding=sf::Encoding::SrgbColor,bool opaque=true){
  std::vector<std::byte> pixels(std::size_t(dimension)*dimension*4,std::byte{255});
  for(std::size_t i=0;i<pixels.size();i+=4){pixels[i]=std::byte(i&255);pixels[i+3]=opaque?std::byte{255}:std::byte{128};}
  auto encoded=sf::encode_texture(id,stamp(),dimension,dimension,encoding,pixels);CHECK(encoded);return std::move(encoded).value();
}
std::vector<std::byte> mip_texture(AssetId id={102,1},std::uint32_t dimension=2){
  std::size_t count{};for(auto level=dimension;;level=std::max(1u,level/2)){count+=std::size_t(level)*level*4;if(level==1)break;}
  std::vector<std::byte> texels(count,std::byte{255});
  for(std::size_t i=0;i<texels.size();i+=4){texels[i]=std::byte((i/4)&255);texels[i+1]=std::byte{37};}
  auto encoded=sf::encode_texture_mips(id,stamp(),dimension,dimension,sf::Encoding::SrgbColor,texels);CHECK(encoded);return std::move(encoded).value();
}
std::vector<std::byte> material(AssetId id={93,1},std::optional<d::MeshReference> ref={},std::array<double,3> color={1,1,1},sf::Sampling sampling=sf::Sampling::LinearClampMip0,sf::TextureRole role=sf::TextureRole::Color){
  sf::Material mat;mat.base_color=color;mat.sampling=sampling;mat.texture_role=role;if(ref)mat.base_color_texture=sf::Reference{ref->asset,ref->revision};
  auto provenance=stamp();provenance.dependencies=sf::dependency_digest(mat.base_color_texture);
  auto encoded=sf::encode_material(id,provenance,mat);CHECK(encoded);return std::move(encoded).value();
}
d::MeshReference put_texture(FakeFiles& files,const std::vector<std::byte>& bytes){
  auto view=sf::validate_texture(bytes);CHECK(view);files.data[assets::hex(view.value().revision())+".rasset"]=bytes;return {view.value().id(),view.value().revision()};
}
d::MaterialReference put_material(FakeFiles& files,const std::vector<std::byte>& bytes){
  auto view=sf::validate_material(bytes);CHECK(view);files.data[assets::hex(view.value().revision())+".rasset"]=bytes;return {view.value().id(),view.value().revision()};
}
std::unique_ptr<d::Document> document(){auto result=d::Document::create();CHECK(result);return std::move(result).value();}
void scene_history_and_persistence(){
  auto files=std::make_shared<FakeFiles>();const auto first=put_material(*files,material());
  const auto second=put_material(*files,material({93,1},{},{.25,.5,.75}));auto doc=document();const auto id=doc->objects().value()[0].id;
  CHECK(doc->assign_material(id,first));CHECK(doc->objects().value()[0].material==first);
  const auto assigned=doc->generation();CHECK(doc->assign_material(id,first));CHECK(doc->generation()==assigned);
  CHECK(doc->assign_material(id,second));CHECK(doc->undo());CHECK(doc->objects().value()[0].material==first);
  CHECK(doc->redo());CHECK(doc->objects().value()[0].material==second);
  CHECK(doc->assign_material(id,{}));CHECK(!doc->objects().value()[0].material);CHECK(doc->undo());
  CHECK(doc->objects().value()[0].material==second);
  const auto before=doc->generation();CHECK(!doc->assign_material(id,d::MaterialReference{}));CHECK(doc->generation()==before);
  CHECK(!doc->assign_material({999,999},first));CHECK(doc->generation()==before);
  CHECK(doc->save(*files,"scene.json"));CHECK(doc->cook(*files,"scene.rscene"));
  auto loaded=document();CHECK(loaded->load(*files,"scene.json"));CHECK(loaded->objects().value()[0].material==second);
  CHECK(loaded->load(*files,"scene.rscene"));CHECK(loaded->objects().value()[0].material==second);
  CHECK(loaded->cook(*files,"copy.rscene"));CHECK(files->data["copy.rscene"]==files->data["scene.rscene"]);
  CHECK(doc->start_play());CHECK(doc->objects(true).value()[0].material==second);CHECK_ERROR(doc->assign_material(id,first),ErrorCode::Busy);CHECK(doc->stop_play());
  auto snapshot=doc->snapshot().value();auto duplicate=snapshot;duplicate.objects[0].components.push_back(duplicate.objects[0].components.back());
  CHECK(!doc->install(duplicate,doc->persistence_stamp()));CHECK(doc->generation()==before);
  auto orphan=snapshot;std::erase_if(orphan.objects[0].components,[](const auto& c){return c.type==d::box_type;});CHECK(!doc->install(orphan,doc->persistence_stamp()));
  auto malformed=snapshot;malformed.objects[0].components.back().fields.pop_back();CHECK(!d::material_references(malformed));
  CHECK(doc->assign_material(id,{}));CHECK(doc->objects().value()[0].visible);
  auto legacy=scene::Schemas::create({{d::box_type,1,"Box",{{1,"dimensions",scene::Vec3{1,1,1},.01,1000,true},{2,"color",scene::Vec3{.28,.52,.78},0,1,true}}},
    {d::mesh_type,1,"Mesh",{{1,"asset",PersistentId{}},{2,"revisionLow",PersistentId{}},{3,"revisionHigh",PersistentId{}},{4,"color",scene::Vec3{.28,.52,.78},0,1,true}}}});CHECK(legacy);
  auto old=scene::cook(doc->snapshot().value(),*legacy.value(),{},"race-desktop-mesh-schema-1");CHECK(old);
  auto decoded=d::Document::decode(old.value().bytes,*doc->schemas());CHECK(decoded&&d::material_references(decoded.value()).value().empty());
}
void owned_packets_and_reuse(){
  auto files=std::make_shared<FakeFiles>();const auto tex=put_texture(*files,texture());
  const auto a=put_material(*files,material({93,1},tex));const auto b=put_material(*files,material({93,2},tex,{.25,.5,.75}));
  const std::array refs{a,b,a};auto set=d::MeshSet::prepare(files,{}, {},refs);CHECK(set);
  CHECK(set.value()->materials().size()==2&&set.value()->textures().size()==1&&files->reads==3);
  auto first=set.value()->find_material(a).value()->material;auto second=set.value()->find_material(b).value()->material;
  CHECK(first->texture()==second->texture());CHECK(second->base_color()==std::array<float,4>{.25f,.5f,.75f,1});
  CHECK(first->texture()->width()==2&&first->texture()->height()==2&&first->texture()->pixels().size()==16);
  auto repeated=d::MeshSet::prepare(files,{},set.value(),refs);CHECK(repeated&&files->reads==3);
  CHECK(repeated.value()->find_material(a).value()->material==first);
  auto other=std::make_shared<FakeFiles>();CHECK(!d::MeshSet::prepare(other,{},set.value(),refs));
  CHECK(d::material_file_reference(*files,assets::hex(a.revision)+".rasset").value()==a);
  CHECK(!d::material_file_reference(*files,"../escape.rasset"));
  const auto updated=material({93,1},tex,{.5,.25,1});const auto reads=files->reads;
  auto extended=d::MeshSet::extend_material(set.value(),updated);CHECK(extended&&files->reads==reads);
  CHECK(extended.value()->materials().size()==3&&extended.value()->textures().size()==1);
  auto view=sf::validate_material(updated).value();d::MaterialReference next{view.id(),view.revision()};
  CHECK(extended.value()->find_material(a).value()->material==first);
  CHECK(d::MeshSet::extend_material(extended.value(),updated).value()==extended.value());
  auto corrupt=updated;corrupt.back()^=std::byte{1};CHECK(!d::MeshSet::extend_material(extended.value(),corrupt));
  CHECK(set.value()->materials().size()==2&&!set.value()->find_material(next));
  auto doc=document();CHECK(doc->assign_material(doc->objects().value()[0].id,a));CHECK(set.value()->validate_scene(doc->snapshot().value()));
  auto frame=d::scene_frame(*doc,set.value().get(),{});CHECK(frame&&frame.value().draws[0].material==first);
  files->data.clear();set.value().reset();extended.value().reset();repeated.value().reset();files.reset();
  CHECK(frame.value().draws[0].material->texture()->pixels()[3]==std::byte{255});
}
void invalid_binding_and_bounds(){
  auto files=std::make_shared<FakeFiles>();const auto data=put_texture(*files,texture({92,1},2,sf::Encoding::LinearData));
  auto ref=put_material(*files,material({93,1},data));CHECK(!d::MeshSet::prepare(files,{}, {},std::span(&ref,1)));
  const auto alpha=put_texture(*files,texture({92,2},2,sf::Encoding::SrgbColor,false));ref=put_material(*files,material({93,2},alpha));CHECK(!d::MeshSet::prepare(files,{}, {},std::span(&ref,1)));
  const auto valid=put_texture(*files,texture({92,3}));ref=put_material(*files,material({93,3},valid));
  auto good=d::MeshSet::prepare(files,{}, {},std::span(&ref,1));CHECK(good);
  auto wrong=ref;++wrong.asset.low;CHECK(!d::MeshSet::prepare(files,{}, {},std::span(&wrong,1)));
  const auto leaf=assets::hex(valid.revision)+".rasset";files->data.erase(leaf);CHECK(!d::MeshSet::prepare(files,{}, {},std::span(&ref,1)));
  CHECK(d::MeshSet::prepare(files,{},good.value(),std::span(&ref,1)));
  const auto source=std::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3");
  auto imported=mesh_pipeline::import_obj({91,1},std::as_bytes(std::span(source)));CHECK(imported);auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);
  auto mesh=m::validate(cooked.value()).value();d::MeshReference mesh_ref{mesh.id(),mesh.revision()};files->data[assets::hex(mesh_ref.revision)+".rasset"]=cooked.value();
  auto set=d::MeshSet::prepare(files,std::span(&mesh_ref,1),good.value(),std::span(&ref,1));CHECK(set);
  auto doc=document();auto id=doc->add_mesh(mesh_ref);CHECK(id);CHECK(doc->assign_material(id.value(),ref));
  CHECK_ERROR(set.value()->validate_scene(doc->snapshot().value()),ErrorCode::InvalidInput);CHECK(!d::scene_frame(*doc,set.value().get(),{}));
  const auto plain=material({93,4});auto solid=d::MeshSet::extend_material(set.value(),plain);CHECK(solid);
  auto plain_view=sf::validate_material(plain).value();CHECK(doc->assign_material(id.value(),d::MaterialReference{plain_view.id(),plain_view.revision()}));
  CHECK(solid.value()->validate_scene(doc->snapshot().value()));CHECK(d::scene_frame(*doc,solid.value().get(),{}));
  std::array<m::Vertex,3> vertices;for(std::size_t i=0;i<vertices.size();++i)vertices[i]=mesh.vertex(i).value();
  vertices[0].uv={0,0};vertices[1].uv={1,0};vertices[2].uv={0,1};const std::array<std::uint32_t,3> indices{0,1,2};
  auto uv_bytes=m::encode(mesh.id(),mesh.stamp(),vertices,indices,{},true);CHECK(uv_bytes);
  auto uv=m::validate(uv_bytes.value()).value();d::MeshReference uv_ref{uv.id(),uv.revision()};
  auto uv_set=d::MeshSet::extend(solid.value(),uv_bytes.value());CHECK(uv_set);
  CHECK(uv_set.value()->materials().size()==2&&uv_set.value()->textures().size()==1);
  CHECK(uv_set.value()->find(uv_ref).value()->geometry->has_uv());
  CHECK(uv_set.value()->find(uv_ref).value()->geometry->vertices()[2].uv==std::array<double,2>{0,1});
  auto uv_doc=document();auto uv_id=uv_doc->add_mesh(uv_ref);CHECK(uv_id);CHECK(uv_doc->assign_material(uv_id.value(),ref));
  CHECK(uv_set.value()->validate_scene(uv_doc->snapshot().value()));CHECK(d::scene_frame(*uv_doc,uv_set.value().get(),{}));
  std::vector<d::MaterialReference> refs(render::viewport_max_draws+1,ref);CHECK_ERROR(d::MeshSet::prepare(files,{}, {},refs),ErrorCode::Exhausted);
  refs.clear();for(std::uint64_t i=1;i<=129;++i)refs.push_back(put_material(*files,material({94,i})));
  CHECK_ERROR(d::MeshSet::prepare(files,{}, {},refs),ErrorCode::Exhausted);refs.resize(128);CHECK(d::MeshSet::prepare(files,{}, {},refs));
  refs.clear();for(std::uint64_t i=1;i<=17;++i){auto t=put_texture(*files,texture({95,i}));refs.push_back(put_material(*files,material({96,i},t)));}
  CHECK_ERROR(d::MeshSet::prepare(files,{}, {},refs),ErrorCode::Exhausted);refs.resize(16);CHECK(d::MeshSet::prepare(files,{}, {},refs));
}
void scalar_mips_rejected_without_publication(){
  auto files=std::make_shared<FakeFiles>();
  const std::array texels{std::byte{0},std::byte{64},std::byte{255},std::byte{255},std::byte{255},std::byte{192},std::byte{0},std::byte{255},
      std::byte{128},std::byte{128},std::byte{128},std::byte{255}};
  auto encoded=sf::encode_texture_scalar_mips({112,1},stamp(),2,1,sf::Encoding::LinearData,texels);CHECK(encoded);
  const auto scalar=put_texture(*files,encoded.value());const auto stable=put_material(*files,material({113,1}));
  auto retained=d::MeshSet::prepare(files,{}, {},std::span(&stable,1));CHECK(retained);
  const auto packet=retained.value()->find_material(stable).value()->material;
  for(const auto sampling:{sf::Sampling::LinearClampMip0,sf::Sampling::LinearClampMips}){
    const auto candidate=material({113,2},scalar,{1,1,1},sampling);const auto ref=put_material(*files,candidate);
    CHECK(!d::MeshSet::prepare(files,{}, {},std::span(&ref,1)));
    CHECK(!d::MeshSet::prepare(files,{},retained.value(),std::span(&ref,1)));
    CHECK(!d::MeshSet::extend_material(retained.value(),candidate));
    CHECK(retained.value()->materials().size()==1&&retained.value()->textures().empty()&&retained.value()->find_material(stable).value()->material==packet);
  }
}
void scalar_roles_and_cached_binding(){
  auto files=std::make_shared<FakeFiles>();
  const std::array texels{std::byte{0},std::byte{64},std::byte{255},std::byte{255},std::byte{255},std::byte{192},std::byte{0},std::byte{255},
      std::byte{128},std::byte{128},std::byte{128},std::byte{255}};
  auto encoded=sf::encode_texture_scalar_mips({114,1},stamp(),2,1,sf::Encoding::LinearData,texels);CHECK(encoded);
  const auto scalar=put_texture(*files,encoded.value());
  const auto a=put_material(*files,material({115,1},scalar,{1,1,1},sf::Sampling::LinearClampMips,sf::TextureRole::DiffuseReflectanceR));
  const auto b=put_material(*files,material({115,2},scalar,{.25,.5,.75},sf::Sampling::LinearClampMips,sf::TextureRole::DiffuseReflectanceR));
  const std::array refs{a,b,a};auto set=d::MeshSet::prepare(files,{}, {},refs);CHECK(set&&files->reads==3);
  CHECK(set.value()->materials().size()==2&&set.value()->textures().size()==1);
  const auto first=set.value()->find_material(a).value()->material,second=set.value()->find_material(b).value()->material;
  CHECK(first->texture_role()==render::TextureRole::DiffuseReflectanceR&&first->sampling()==render::TextureSampling::LinearClampMips);
  CHECK(first->texture()==second->texture()&&first->texture()->encoding()==render::TextureEncoding::LinearData);
  CHECK(first->texture()->complete_mip_chain()&&first->texture()->mip_count()==2&&first->texture()->texels().size()==12);
  const auto reads=files->reads;files->throw_read=true;
  auto repeated=d::MeshSet::prepare(files,{},set.value(),refs);CHECK(repeated&&files->reads==reads);
  CHECK(repeated.value()->find_material(a).value()->material==first);
  const auto next_bytes=material({115,3},scalar,{.5,.25,1},sf::Sampling::LinearClampMips,sf::TextureRole::DiffuseReflectanceR);
  auto next=d::MeshSet::extend_material(set.value(),next_bytes);CHECK(next&&files->reads==reads);
  CHECK(next.value()->textures()[0].texture==first->texture()&&next.value()->materials().size()==3);
  // Exact same texture ID+revision is already resident. Legacy Color must still
  // fail at role admission, including when deduplicated earlier in this load.
  files->throw_read=false;
  for(const auto sampling:{sf::Sampling::LinearClampMip0,sf::Sampling::LinearClampMips}){
    const auto wrong=material({115,4},scalar,{1,1,1},sampling);const auto bad=put_material(*files,wrong);
    const auto before=files->reads;
    CHECK(!d::MeshSet::extend_material(set.value(),wrong));CHECK(files->reads==before);
    CHECK(!d::MeshSet::prepare(files,{},set.value(),std::span(&bad,1)));CHECK(files->reads==before+1);
    const std::array same_load{a,bad};CHECK(!d::MeshSet::prepare(files,{}, {},same_load));
    CHECK(set.value()->materials().size()==2&&set.value()->textures().size()==1&&set.value()->find_material(a).value()->material==first);
  }
  // Conversely, a retained sRGB chain cannot satisfy the scalar role.
  const auto srgb=put_texture(*files,mip_texture({114,2}));const auto color=put_material(*files,material({115,5},srgb,{1,1,1},sf::Sampling::LinearClampMips));
  auto color_set=d::MeshSet::prepare(files,{}, {},std::span(&color,1));CHECK(color_set);
  CHECK(!d::MeshSet::extend_material(color_set.value(),material({115,6},srgb,{1,1,1},sf::Sampling::LinearClampMips,sf::TextureRole::DiffuseReflectanceR)));
  // Both schema-1 cooked and imported LinearData are distinct from scalar chains.
  for(const bool imported:{false,true}){
    auto provenance=stamp();if(imported){provenance.representation=sf::Representation::Imported;provenance.target=0;provenance.cooker={};}
    auto old=sf::encode_texture({114,3},provenance,1,1,sf::Encoding::LinearData,std::span(texels).first(4));CHECK(old);
    const auto old_ref=put_texture(*files,old.value());const auto bad=put_material(*files,
        material({115,7},old_ref,{1,1,1},sf::Sampling::LinearClampMips,sf::TextureRole::DiffuseReflectanceR));
    CHECK(!d::MeshSet::prepare(files,{},set.value(),std::span(&bad,1)));
  }
  auto doc=document();CHECK(doc->assign_material(doc->objects().value()[0].id,a));
  CHECK(set.value()->validate_scene(doc->snapshot().value()));auto frame=d::scene_frame(*doc,set.value().get(),{});CHECK(frame);
  CHECK(frame.value().draws[0].material==first);
  files->data.clear();set.value().reset();next.value().reset();repeated.value().reset();
  CHECK(frame.value().draws[0].material->texture()->mip(1)->pixels[0]==std::byte{128});
}
void payload_capacity(){
  auto files=std::make_shared<FakeFiles>();std::vector<d::MaterialReference> refs;
  for(std::uint64_t i=1;i<=9;++i){auto t=put_texture(*files,texture({97,i},1024));refs.push_back(put_material(*files,material({98,i},t)));}
  CHECK_ERROR(d::MeshSet::prepare(files,{}, {},refs),ErrorCode::Exhausted);refs.resize(8);
  auto exact=d::MeshSet::prepare(files,{}, {},refs);CHECK(exact&&exact.value()->textures().size()==8);
  std::size_t bytes{};for(const auto& t:exact.value()->textures())bytes+=t.texture->pixels().size();CHECK(bytes==render::viewport_texture_frame_bytes);
}
void mip_profiles_and_retained_bindings(){
  auto files=std::make_shared<FakeFiles>();auto product=mip_texture();const auto tex=put_texture(*files,product);
  const auto legacy=put_material(*files,material({103,1},tex));
  const auto filtered_bytes=material({103,2},tex,{1,1,1},sf::Sampling::LinearClampMips);
  const auto filtered=put_material(*files,filtered_bytes);const std::array refs{legacy,filtered,legacy};
  auto set=d::MeshSet::prepare(files,{}, {},refs);CHECK(set&&files->reads==3);
  CHECK(set.value()->materials().size()==2&&set.value()->textures().size()==1);
  auto mip0=set.value()->find_material(legacy).value()->material;auto mips=set.value()->find_material(filtered).value()->material;
  CHECK(mip0->sampling()==render::TextureSampling::LinearClampMip0&&mips->sampling()==render::TextureSampling::LinearClampMips);
  CHECK(mip0->texture()==mips->texture()&&mips->texture()->complete_mip_chain());
  CHECK(mips->texture()->mip_count()==2&&mips->texture()->pixels().size()==16&&mips->texture()->texels().size()==20);
  const auto expected=std::vector<std::byte>(mips->texture()->texels().begin(),mips->texture()->texels().end());
  auto reverse=d::MeshSet::prepare(files,{}, {},std::array{filtered,legacy});CHECK(reverse);
  CHECK(reverse.value()->materials().size()==2&&reverse.value()->textures().size()==1);
  auto repeated=d::MeshSet::prepare(files,{},set.value(),refs);const auto reads=files->reads;CHECK(repeated);
  CHECK(repeated.value()->find_material(filtered).value()->material==mips);
  auto legacy_only=d::MeshSet::prepare(files,{},set.value(),std::span(&legacy,1));CHECK(legacy_only&&files->reads==reads);
  auto added=d::MeshSet::extend_material(legacy_only.value(),filtered_bytes);CHECK(added&&files->reads==reads);
  CHECK(added.value()->find_material(filtered).value()->material->texture()==mip0->texture());
  auto one=put_texture(*files,mip_texture({102,2},1));auto one_ref=put_material(*files,material({103,3},one,{1,1,1},sf::Sampling::LinearClampMips));
  auto one_set=d::MeshSet::prepare(files,{}, {},std::span(&one_ref,1));CHECK(one_set);
  auto one_packet=one_set.value()->find_material(one_ref).value()->material->texture();
  CHECK(one_packet->complete_mip_chain()&&one_packet->mip_count()==1&&one_packet->texels().size()==4);
  // Reusing a validated legacy texture cannot satisfy an explicit mip profile,
  // even when its dimensions are already 1x1 and its mip count is one.
  const auto old_tex=put_texture(*files,texture({102,3},1));const auto old_ref=put_material(*files,material({103,4},old_tex));
  const auto invalid=material({103,5},old_tex,{1,1,1},sf::Sampling::LinearClampMips);
  const auto invalid_ref=put_material(*files,invalid);auto retained=d::MeshSet::prepare(files,{}, {},std::span(&old_ref,1));CHECK(retained);
  const auto retained_packet=retained.value()->find_material(old_ref).value()->material;
  CHECK(!d::MeshSet::prepare(files,{}, {},std::array{old_ref,invalid_ref}));
  CHECK(!d::MeshSet::prepare(files,{}, {},std::array{invalid_ref,old_ref}));
  CHECK(!d::MeshSet::prepare(files,{},retained.value(),std::span(&invalid_ref,1)));
  CHECK(!d::MeshSet::extend_material(retained.value(),invalid));
  CHECK(retained.value()->materials().size()==1&&retained.value()->find_material(old_ref).value()->material==retained_packet);
  // A checksum-valid nonopaque nonzero mip is rejected before any packet publication.
  auto alpha=product;alpha[sf::header_bytes+19]=std::byte{128};
  auto checksum=assets::digest(std::span<const std::byte>(alpha).first(alpha.size()-sf::checksum_bytes));std::copy(checksum.begin(),checksum.end(),alpha.end()-sf::checksum_bytes);
  const d::MeshReference alpha_ref{tex.asset,assets::digest(alpha)};files->data[assets::hex(alpha_ref.revision)+".rasset"]=alpha;
  auto alpha_mat=put_material(*files,material({103,6},alpha_ref,{1,1,1},sf::Sampling::LinearClampMips));
  CHECK(!d::MeshSet::prepare(files,{}, {},std::span(&alpha_mat,1)));
  auto doc=document();CHECK(doc->assign_material(doc->objects().value()[0].id,filtered));auto frame=d::scene_frame(*doc,set.value().get(),{});CHECK(frame);
  std::fill(product.begin(),product.end(),std::byte{});files->data.clear();set.value().reset();reverse.value().reset();repeated.value().reset();added.value().reset();legacy_only.value().reset();mip0.reset();mips.reset();files.reset();
  CHECK(std::equal(frame.value().draws[0].material->texture()->texels().begin(),frame.value().draws[0].material->texture()->texels().end(),expected.begin(),expected.end()));
}
void mip_payload_capacity(){
  auto files=std::make_shared<FakeFiles>();std::vector<d::MaterialReference> refs;std::vector<d::MeshReference> pins;
  for(std::uint64_t i=1;i<=7;++i){pins.push_back(put_texture(*files,mip_texture({104,i},1024)));refs.push_back(put_material(*files,material({105,i},pins.back(),{1,1,1},sf::Sampling::LinearClampMips)));}
  CHECK_ERROR(d::MeshSet::prepare(files,{}, {},refs),ErrorCode::Exhausted);
  auto six=d::MeshSet::prepare(files,{}, {},std::span(refs).first(6));CHECK(six&&six.value()->textures().size()==6);
  std::size_t charge{};for(const auto& t:six.value()->textures())charge+=t.texture->texels().size();
  CHECK(charge==33554424&&render::viewport_texture_frame_bytes-charge==8);
  const auto retained=six.value()->find_material(refs.front()).value()->material;
  CHECK_ERROR(d::MeshSet::prepare(files,{},six.value(),refs),ErrorCode::Exhausted);
  const auto seventh=files->data[assets::hex(refs.back().revision)+".rasset"];
  CHECK_ERROR(d::MeshSet::extend_material(six.value(),seventh),ErrorCode::Exhausted);
  auto shared=material({105,8},pins.front());auto shared_set=d::MeshSet::extend_material(six.value(),shared);CHECK(shared_set&&shared_set.value()->textures().size()==6);
  CHECK(shared_set.value()->find_material(refs.front()).value()->material==retained);
  // Eight bytes remain after six full chains: two explicit 1x1 chains fit,
  // a third does not. This catches base-only charging in both cached paths.
  refs.resize(6);for(std::uint64_t i=1;i<=3;++i){auto t=put_texture(*files,mip_texture({106,i},1));refs.push_back(put_material(*files,material({107,i},t,{1,1,1},sf::Sampling::LinearClampMips)));}
  auto exact=d::MeshSet::prepare(files,{},six.value(),std::span(refs).first(8));CHECK(exact&&exact.value()->textures().size()==8);
  charge=0;for(const auto& t:exact.value()->textures())charge+=t.texture->texels().size();CHECK(charge==render::viewport_texture_frame_bytes);
  CHECK_ERROR(d::MeshSet::prepare(files,{},exact.value(),refs),ErrorCode::Exhausted);
  CHECK_ERROR(d::MeshSet::extend_material(exact.value(),files->data[assets::hex(refs.back().revision)+".rasset"]),ErrorCode::Exhausted);
  CHECK(six.value()->textures().size()==6&&six.value()->find_material(refs.front()).value()->material==retained);
  six.value().reset();exact.value().reset();shared_set.value().reset();files->data.clear();refs.clear();
  for(std::uint64_t i=1;i<=17;++i){auto t=put_texture(*files,mip_texture({108,i},1));refs.push_back(put_material(*files,material({109,i},t,{1,1,1},sf::Sampling::LinearClampMips)));}
  auto sixteen=d::MeshSet::prepare(files,{}, {},std::span(refs).first(16));CHECK(sixteen&&sixteen.value()->textures().size()==16);
  CHECK_ERROR(d::MeshSet::prepare(files,{}, {},refs),ErrorCode::Exhausted);
  CHECK_ERROR(d::MeshSet::prepare(files,{},sixteen.value(),refs),ErrorCode::Exhausted);
  CHECK_ERROR(d::MeshSet::extend_material(sixteen.value(),files->data[assets::hex(refs.back().revision)+".rasset"]),ErrorCode::Exhausted);
}
std::vector<pak::Input> package_inputs(std::vector<std::byte>& bytes){
  auto entries=pak::validate(bytes);CHECK(entries);std::vector<pak::Input> inputs;
  for(const auto& e:entries.value())inputs.push_back({e.key.id,e.kind,std::span(bytes).subspan(static_cast<std::size_t>(e.offset),static_cast<std::size_t>(e.bytes)),e.dependencies,e.schema});return inputs;
}
void exact_package_closure(){
  auto files=std::make_shared<FakeFiles>();const auto tex=put_texture(*files,texture());const auto mat=put_material(*files,material({93,1},tex));
  auto doc=document();const auto id=doc->objects().value()[0].id;CHECK(doc->assign_material(id,mat));
  const auto source=std::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3");
  auto imported=mesh_pipeline::import_obj({91,2},std::as_bytes(std::span(source)));CHECK(imported);auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);
  auto mesh=m::validate(cooked.value()).value();std::array<m::Vertex,3> vertices;
  for(std::size_t i=0;i<vertices.size();++i)vertices[i]=mesh.vertex(i).value();vertices[1].uv={1,0};vertices[2].uv={0,1};
  const std::array<std::uint32_t,3> indices{0,1,2};auto uv=m::encode(mesh.id(),mesh.stamp(),vertices,indices,{},true);CHECK(uv);
  auto uv_view=m::validate(uv.value()).value();d::MeshReference mesh_ref{uv_view.id(),uv_view.revision()};
  files->data[assets::hex(mesh_ref.revision)+".rasset"]=uv.value();auto mesh_id=doc->add_mesh(mesh_ref);CHECK(mesh_id);CHECK(doc->assign_material(mesh_id.value(),mat));
  CHECK(doc->cook(*files,"scene.rscene"));
  auto packed=d::pack_scene(*files,"scene.rscene",{99,1},*doc->schemas());CHECK(packed);CHECK(d::pack_scene(*files,"scene.rscene",{99,1},*doc->schemas()).value()==packed.value());
  auto prepared=d::prepare_package(packed.value(),*doc->schemas());CHECK(prepared&&prepared.value().meshes->materials().size()==1&&prepared.value().meshes->textures().size()==1);
  CHECK(pak::validate(packed.value()).value().size()==4);
  auto target=document();CHECK(target->install(prepared.value().scene,target->persistence_stamp()));auto frame=d::scene_frame(*target,prepared.value().meshes.get(),{});CHECK(frame&&frame.value().draws[0].material->texture());
  auto inputs=package_inputs(packed.value());auto modified=inputs;
  auto mesh_input=std::find_if(modified.begin(),modified.end(),[](const auto& item){return item.kind==pak::Kind::Mesh;});
  CHECK(mesh_input!=modified.end()&&mesh_input->schema==2);mesh_input->schema=1;auto mislabelled=pak::encode(modified);CHECK(mislabelled);
  CHECK_ERROR(d::prepare_package(mislabelled.value(),*doc->schemas()),ErrorCode::InvalidDependency);modified=inputs;
  auto find_material=[](auto& list){return std::find_if(list.begin(),list.end(),[](const auto& item){return item.kind==pak::Kind::Material;});};
  find_material(modified)->dependencies.clear();auto bad=pak::encode(modified);CHECK(bad);CHECK_ERROR(d::prepare_package(bad.value(),*doc->schemas()),ErrorCode::InvalidDependency);
  modified=inputs;auto texture_input=std::find_if(modified.begin(),modified.end(),[](const auto& item){return item.kind==pak::Kind::Texture;});texture_input->kind=pak::Kind::Mesh;CHECK(!pak::encode(modified));
  modified=inputs;std::erase_if(modified,[](const auto& item){return item.kind==pak::Kind::Texture;});CHECK(!pak::encode(modified));
  modified=inputs;auto extra=*std::find_if(modified.begin(),modified.end(),[](const auto& item){return item.kind==pak::Kind::Texture;});extra.id={99,2};modified.push_back(extra);
  bad=pak::encode(modified);CHECK(bad);CHECK_ERROR(d::prepare_package(bad.value(),*doc->schemas()),ErrorCode::InvalidDependency);
  modified=inputs;find_material(modified)->kind=pak::Kind::Mesh;
  CHECK(!pak::encode(modified));
  const auto generation=target->generation();CHECK(!d::prepare_package(bad.value(),*doc->schemas()));CHECK(target->generation()==generation);
  files->data.clear();packed.value().clear();prepared.value().meshes.reset();CHECK(frame.value().draws[0].material->texture()->pixels().size()==16);
}
}
int main(){scene_history_and_persistence();owned_packets_and_reuse();invalid_binding_and_bounds();scalar_mips_rejected_without_publication();scalar_roles_and_cached_binding();payload_capacity();mip_profiles_and_retained_bindings();mip_payload_capacity();exact_package_closure();
  std::puts("desktop material Scene history, owned texture reuse, bounds and typed exact package closure passed");}
