#include "desktop_package.hpp"
#include "desktop_persistence.hpp"
#include "mesh_pipeline.hpp"
#include "assets_support.hpp"

namespace {
namespace d=race::desktop;
namespace p=race::assets::pak;
namespace m=race::assets::mesh;
struct FixturePackage {
  std::shared_ptr<FakeFiles> files=std::make_shared<FakeFiles>();
  std::unique_ptr<d::Document> doc=std::move(d::Document::create()).value();
  std::vector<d::MeshReference> refs;
  FixturePackage(){
    for(auto source:{"v 2 0 0\nv 4 0 0\nv 2 2 0\nf 1 2 3","v 2 0 0\nv 5 0 0\nv 2 2 0\nf 1 2 3"}){
      auto text=std::string_view(source);auto imported=mesh_pipeline::import_obj({91,7},std::as_bytes(std::span(text)));CHECK(imported);
      auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);auto view=m::validate(cooked.value());CHECK(view);
      refs.push_back({view.value().id(),view.value().revision()});files->data[assets::hex(refs.back().revision)+".rasset"]=cooked.value();CHECK(doc->add_mesh(refs.back()));
    }
    CHECK(doc->add_mesh(refs[0]));CHECK(doc->cook(*files,"scene.rscene"));
  }
  std::vector<std::byte> pack(){auto b=d::pack_scene(*files,"scene.rscene",{1,2},*doc->schemas());CHECK(b);return std::move(b).value();}
};
std::vector<p::Input> inputs_from(std::vector<std::byte>& bytes){
  auto entries=p::validate(bytes);CHECK(entries);std::vector<p::Input> out;
  for(const auto& e:entries.value())out.push_back({e.key.id,e.kind,std::span(bytes).subspan(static_cast<std::size_t>(e.offset),static_cast<std::size_t>(e.bytes)),e.dependencies,e.schema});return out;
}
void roundtrip_and_lifetime(){
  FixturePackage f;auto bytes=f.pack();CHECK(f.pack()==bytes);CHECK(f.refs[0].asset==f.refs[1].asset&&f.refs[0].revision!=f.refs[1].revision);
  auto prepared=d::prepare_package(bytes,*f.doc->schemas());CHECK(prepared&&prepared.value().meshes->entries().size()==2);
  auto target=std::move(d::Document::create()).value();CHECK(target->install(prepared.value().scene,target->persistence_stamp()));
  CHECK(target->cook(*f.files,"repeat.rscene"));CHECK(f.files->data["scene.rscene"]==f.files->data["repeat.rscene"]);
  CHECK(d::mesh_references(prepared.value().scene).value()==std::vector<d::MeshReference>{f.refs[0],f.refs[1],f.refs[0]});
  auto frame=d::scene_frame(*target,prepared.value().meshes.get(),{});CHECK(frame&&frame.value().draws.size()==4);
  CHECK(target->start_play());CHECK(target->tick_play(1.0/60));CHECK(target->stop_play());CHECK(target->cook(*f.files,"after.rscene"));CHECK(f.files->data["after.rscene"]==f.files->data["scene.rscene"]);
  auto snapshot_files=prepared.value().meshes->files();std::weak_ptr<platform::RootedFiles> weak=snapshot_files;
  f.files->data.clear();bytes.clear();CHECK(snapshot_files->read(assets::hex(f.refs[0].revision)+".rasset",16*1024*1024));
  // New intentionally releases the content source as well as prepared entries.
  // Independently owned frame packets remain valid through their consumer use.
  snapshot_files.reset();CHECK(target->reset());prepared.value().meshes=d::MeshSet::prepare(nullptr,{}).value();CHECK(weak.expired());
  CHECK(frame.value().draws[1].geometry&&frame.value().draws[1].geometry->vertices().size()==3);
}
void closure_and_preservation(){
  FixturePackage f;auto bytes=f.pack();auto inputs=inputs_from(bytes);auto schemas=f.doc->schemas();
  const auto generation=f.doc->generation();auto original=f.doc->snapshot().value();
  auto bad=bytes;bad.back()^=std::byte{1};CHECK(!d::prepare_package(bad,*schemas));CHECK(f.doc->generation()==generation);
  auto extra=inputs;auto mesh=std::find_if(extra.begin(),extra.end(),[](const auto& e){return e.kind==p::Kind::Mesh;});auto unused=*mesh;unused.id={8,9};extra.push_back(unused);
  auto encoded=p::encode(extra);CHECK(encoded);CHECK_ERROR(d::prepare_package(encoded.value(),*schemas),ErrorCode::InvalidDependency);
  extra=inputs;auto scene=std::find_if(extra.begin(),extra.end(),[](const auto& e){return e.kind==p::Kind::Scene;});scene->dependencies.pop_back();encoded=p::encode(extra);CHECK(encoded);CHECK_ERROR(d::prepare_package(encoded.value(),*schemas),ErrorCode::InvalidDependency);
  extra=inputs;scene=std::find_if(extra.begin(),extra.end(),[](const auto& e){return e.kind==p::Kind::Scene;});auto second=*scene;second.id={8,9};extra.push_back(second);encoded=p::encode(extra);CHECK(encoded);CHECK_ERROR(d::prepare_package(encoded.value(),*schemas),ErrorCode::Unsupported);
  extra=inputs;std::erase_if(extra,[](const auto& e){return e.kind==p::Kind::Scene;});encoded=p::encode(extra);CHECK(encoded);CHECK_ERROR(d::prepare_package(encoded.value(),*schemas),ErrorCode::NotFound);
  std::array opaque{std::byte{1}};p::Input fake{{1,2},p::Kind::Scene,opaque,{}};encoded=p::encode({&fake,1});CHECK(encoded);CHECK_ERROR(d::prepare_package(encoded.value(),*schemas),ErrorCode::InvalidInput);
  CHECK(f.doc->generation()==generation);CHECK(f.doc->undo());CHECK(f.doc->redo());CHECK(d::mesh_references(f.doc->snapshot().value()).value()==d::mesh_references(original).value());
  const auto leaf=assets::hex(f.refs[0].revision)+".rasset";auto correct=f.files->data[leaf];
  f.files->data.erase(leaf);CHECK_ERROR(d::pack_scene(*f.files,"scene.rscene",{1,2},*schemas),ErrorCode::NotFound);
  f.files->data[leaf]=f.files->data[assets::hex(f.refs[1].revision)+".rasset"];CHECK(!d::pack_scene(*f.files,"scene.rscene",{1,2},*schemas));
  f.files->data[leaf]=correct;f.files->data[leaf].back()^=std::byte{1};CHECK(!d::pack_scene(*f.files,"scene.rscene",{1,2},*schemas));
  auto text=std::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3");auto imported=mesh_pipeline::import_obj(f.refs[0].asset,std::as_bytes(std::span(text)));CHECK(imported);
  f.files->data[leaf]=imported.value();CHECK(!d::pack_scene(*f.files,"scene.rscene",{1,2},*schemas));f.files->data[leaf]=correct;
  CHECK(!d::pack_scene(*f.files,"../scene.rscene",{1,2},*schemas));CHECK(!d::pack_scene(*f.files,"scene.rscene",{},*schemas));
  CHECK(f.doc->save(*f.files,"authoring.rscene"));CHECK(!d::pack_scene(*f.files,"authoring.rscene",{1,2},*schemas));
  f.files->throw_read=true;CHECK_ERROR(d::read_package(*f.files,"missing.rpak",*schemas),ErrorCode::Exhausted);
}
void publication(){
  FixturePackage f;auto bytes=f.pack();FakeFiles output;const auto hash=assets::hex(assets::digest(bytes))+".rpak";
  auto first=d::publish_package(output,bytes,*f.doc->schemas());CHECK(first&&first.value()==hash);CHECK(d::publish_package(output,bytes,*f.doc->schemas()).value()==hash);CHECK(output.data.size()==1&&output.data[hash]==bytes);
  output.fail_write=output.writes;CHECK_ERROR(d::publish_package(output,bytes,*f.doc->schemas()),ErrorCode::IoFailure);CHECK(output.data[hash]==bytes);
  output.fail_write=SIZE_MAX;output.data[hash]={std::byte{9}};CHECK_ERROR(d::publish_package(output,bytes,*f.doc->schemas()),ErrorCode::Duplicate);CHECK(output.data[hash]==std::vector<std::byte>{std::byte{9}});
  struct Readback:FakeFiles{bool corrupt{};Result<std::vector<std::byte>> read(std::string_view n,std::size_t limit) override{if(!corrupt)return fail(ErrorCode::IoFailure,"readback failure");auto b=FakeFiles::read(n,limit);if(b)b.value().back()^=std::byte{1};return b;}} failed;
  CHECK_ERROR(d::publish_package(failed,bytes,*f.doc->schemas()),ErrorCode::IoFailure);CHECK(failed.data[hash]==bytes);
  failed.corrupt=true;CHECK_ERROR(d::publish_package(failed,bytes,*f.doc->schemas()),ErrorCode::IoFailure);CHECK(failed.data[hash]==bytes);
  auto bad=bytes;bad.back()^=std::byte{1};const auto writes=failed.writes;CHECK(!d::publish_package(failed,bad,*f.doc->schemas()));CHECK(failed.writes==writes&&failed.data[hash]==bytes);
}
template<class F> void sweep(F operation){
  bool failed=false,passed=false;
  for(std::size_t n=0;n<4096;++n){asset_fail_after(n);auto r=operation();asset_fail_disable();if(r){passed=true;break;}CHECK_ERROR(r,ErrorCode::Exhausted);failed=true;}
  CHECK(failed&&passed);CHECK(operation());
}
void failures_and_decode(){
  FixturePackage f;auto bytes=f.pack();const auto schemas=f.doc->schemas();
  sweep([&]{return d::prepare_package(bytes,*schemas);});sweep([&]{return d::pack_scene(*f.files,"scene.rscene",{1,2},*schemas);});
  FakeFiles output;CHECK(d::publish_package(output,bytes,*schemas));const auto prior=output.data;
  sweep([&]{return d::publish_package(output,bytes,*schemas);});CHECK(output.data==prior);
  auto scene=f.files->data["scene.rscene"];auto decoded=d::Document::decode(scene,*schemas);CHECK(decoded);scene.clear();
  CHECK(d::Document::write(output,"span.rscene",decoded.value(),*schemas,true));CHECK(output.data["span.rscene"]==f.files->data["scene.rscene"]);
  auto empty=std::move(d::Document::create()).value();CHECK(empty->cook(output,"empty.rscene"));auto packed=d::pack_scene(output,"empty.rscene",{3,4},*schemas);CHECK(packed);
  auto prepared=d::prepare_package(packed.value(),*schemas);CHECK(prepared&&prepared.value().meshes->entries().empty());
  // Matching imported ID/revision isolates representation rejection.
  auto text=std::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3");auto imported=mesh_pipeline::import_obj({7,8},std::as_bytes(std::span(text)));CHECK(imported);
  auto view=m::validate(imported.value());CHECK(view);CHECK(empty->add_mesh({view.value().id(),view.value().revision()}));CHECK(empty->cook(output,"imported.rscene"));
  output.data[assets::hex(view.value().revision())+".rasset"]=imported.value();CHECK_ERROR(d::pack_scene(output,"imported.rscene",{3,4},*schemas),ErrorCode::InvalidInput);
  std::array<p::Input,2> entries{{{{3,4},p::Kind::Scene,output.data["imported.rscene"],{{view.value().id(),view.value().revision()}}},{view.value().id(),p::Kind::Mesh,imported.value(),{}}}};
  auto container=p::encode(entries);CHECK(container);CHECK_ERROR(d::prepare_package(container.value(),*schemas),ErrorCode::InvalidInput);
}
void mixed_surface_schemas(){
  namespace sf=race::assets::surface;
  for(std::uint32_t dimension:{1u,2u}){
    FixturePackage f;sf::Stamp stamp;stamp.representation=sf::Representation::Cooked;stamp.target=sf::portable_cpu_target;
    const auto label=std::string_view("package-mip-fixture");stamp.source=stamp.importer=stamp.cooker=stamp.settings=assets::digest(std::as_bytes(std::span(label)));
    std::vector<std::byte> texels(dimension==1?4:20,std::byte{255});texels[0]=std::byte{17};texels[texels.size()-4]=std::byte{91};
    auto chain=sf::encode_texture_mips({120,1},stamp,dimension,dimension,sf::Encoding::SrgbColor,texels);CHECK(chain);
    auto chain_view=sf::validate_texture(chain.value());CHECK(chain_view&&chain_view.value().schema()==2);
    std::array<std::byte,4> base{std::byte{21},std::byte{39},std::byte{57},std::byte{255}};
    auto legacy=sf::encode_texture({120,2},stamp,1,1,sf::Encoding::SrgbColor,base);CHECK(legacy);
    const auto legacy_view=sf::validate_texture(legacy.value()).value();
    f.files->data[assets::hex(chain_view.value().revision())+".rasset"]=chain.value();
    f.files->data[assets::hex(legacy_view.revision())+".rasset"]=legacy.value();
    auto scalar=sf::encode_texture_scalar_mips({120,3},stamp,dimension,dimension,sf::Encoding::LinearData,texels);CHECK(scalar);
    const auto scalar_view=sf::validate_texture(scalar.value());CHECK(scalar_view&&scalar_view.value().schema()==3);
    f.files->data[assets::hex(scalar_view.value().revision())+".rasset"]=scalar.value();
    std::vector<d::MaterialReference> materials;
    for(std::uint64_t n=1;n<=5;++n){
      sf::Material material;const auto& texture=n>=4?scalar_view.value():n==3?legacy_view:chain_view.value();
      material.base_color_texture=sf::Reference{texture.id(),texture.revision()};
      if(n==1||n>=4)material.sampling=sf::Sampling::LinearClampMips;
      if(n>=4)material.texture_role=sf::TextureRole::DiffuseReflectanceR;
      auto material_stamp=stamp;material_stamp.dependencies=sf::dependency_digest(material.base_color_texture);
      auto product=sf::encode_material({121,n},material_stamp,material);CHECK(product);auto view=sf::validate_material(product.value());CHECK(view);
      materials.push_back({view.value().id(),view.value().revision()});f.files->data[assets::hex(view.value().revision())+".rasset"]=product.value();
      auto object=n==1?f.doc->objects().value()[0].id:f.doc->add_box().value();CHECK(f.doc->assign_material(object,materials.back()));
    }
    CHECK(f.doc->assign_material(f.doc->add_box().value(),materials[3]));
    CHECK(f.doc->cook(*f.files,"scene.rscene"));auto bytes=f.pack();CHECK(bytes==f.pack());auto entries=p::validate(bytes);CHECK(entries&&entries.value().size()==11);
    std::size_t schemas[2][3]{};
    for(const auto& e:entries.value())if(e.kind==p::Kind::Texture||e.kind==p::Kind::Material)++schemas[e.kind==p::Kind::Material?1:0][e.schema-1];
    CHECK(schemas[0][0]==1&&schemas[0][1]==1&&schemas[0][2]==1&&schemas[1][0]==2&&schemas[1][1]==1&&schemas[1][2]==2);
    auto prepared=d::prepare_package(bytes,*f.doc->schemas());CHECK(prepared);
    auto meshes=prepared.value().meshes;CHECK(meshes->textures().size()==3&&meshes->materials().size()==5);
    auto filtered=meshes->find_material(materials[0]).value()->material;
    auto mip0=meshes->find_material(materials[1]).value()->material;
    CHECK(filtered->texture()==mip0->texture()&&filtered->sampling()==render::TextureSampling::LinearClampMips);
    CHECK(mip0->sampling()==render::TextureSampling::LinearClampMip0);
    CHECK(filtered->texture()->complete_mip_chain()&&filtered->texture()->texels().size()==texels.size());
    auto scalar_material=meshes->find_material(materials[3]).value()->material;
    CHECK(scalar_material->texture_role()==render::TextureRole::DiffuseReflectanceR&&scalar_material->sampling()==render::TextureSampling::LinearClampMips);
    CHECK(scalar_material->texture()->encoding()==render::TextureEncoding::LinearData&&scalar_material->texture()->complete_mip_chain());
    CHECK(scalar_material->texture()==meshes->find_material(materials[4]).value()->material->texture()&&scalar_material->texture()!=filtered->texture());
    auto target=std::move(d::Document::create()).value();CHECK(target->install(prepared.value().scene,target->persistence_stamp()));
    auto frame=d::scene_frame(*target,meshes.get(),{});CHECK(frame);const auto generation=target->generation();
    const auto pins=d::material_references(target->snapshot().value()).value();
    auto inputs=inputs_from(bytes);
    for(std::size_t i=0;i<inputs.size();++i){
      if(inputs[i].kind!=p::Kind::Texture&&inputs[i].kind!=p::Kind::Material)continue;
      for(const auto schema:{1u,2u,3u})if(schema!=inputs[i].schema){
        auto wrong=inputs;wrong[i].schema=schema;auto encoded=p::encode(wrong);CHECK(encoded);
        CHECK_ERROR(d::prepare_package(encoded.value(),*f.doc->schemas()),ErrorCode::InvalidDependency);
      }
      auto wrong=inputs;wrong[i].schema=4;CHECK_ERROR(p::encode(wrong),ErrorCode::IncompatibleVersion);
    }
    auto bad=inputs;auto mat=std::find_if(bad.begin(),bad.end(),[](const auto& e){return e.kind==p::Kind::Material&&e.schema==2;});
    CHECK(mat!=bad.end());mat->dependencies.clear();auto encoded=p::encode(bad);CHECK(encoded);
    CHECK_ERROR(d::prepare_package(encoded.value(),*f.doc->schemas()),ErrorCode::InvalidDependency);
    bad=inputs;std::erase_if(bad,[](const auto& e){return e.kind==p::Kind::Texture&&e.schema==2;});CHECK_ERROR(p::encode(bad),ErrorCode::NotFound);
    bad=inputs;auto texture=std::find_if(bad.begin(),bad.end(),[](const auto& e){return e.kind==p::Kind::Texture&&e.schema==2;});
    auto unused=*texture;unused.id={122,1};bad.push_back(unused);encoded=p::encode(bad);CHECK(encoded);
    CHECK_ERROR(d::prepare_package(encoded.value(),*f.doc->schemas()),ErrorCode::InvalidDependency);
    bad=inputs;texture=std::find_if(bad.begin(),bad.end(),[](const auto& e){return e.kind==p::Kind::Texture&&e.schema==2;});texture->kind=p::Kind::Mesh;CHECK_ERROR(p::encode(bad),ErrorCode::Unsupported);
    CHECK(target->generation()==generation&&d::material_references(target->snapshot().value()).value()==pins);
    CHECK(meshes->find_material(materials[0]).value()->material==filtered);
    if(dimension==2){
      sweep([&]{return d::prepare_package(bytes,*f.doc->schemas());});
      sweep([&]{return d::pack_scene(*f.files,"scene.rscene",{1,2},*f.doc->schemas());});
      CHECK(target->generation()==generation&&meshes->find_material(materials[0]).value()->material==filtered);
    }
    std::fill(bytes.begin(),bytes.end(),std::byte{});f.files->data.clear();chain.value().clear();legacy.value().clear();prepared.value().meshes.reset();meshes.reset();filtered.reset();mip0.reset();scalar_material.reset();scalar.value().clear();
    std::size_t scalar_draws{};std::shared_ptr<const render::ViewportTexture> retained_texture;
    for(const auto& draw:frame.value().draws)if(draw.material&&draw.material->texture_role()==render::TextureRole::DiffuseReflectanceR){
      ++scalar_draws;if(retained_texture)CHECK(draw.material->texture()==retained_texture);else retained_texture=draw.material->texture();
      CHECK(draw.material->texture()->encoding()==render::TextureEncoding::LinearData);
      CHECK(std::equal(draw.material->texture()->texels().begin(),draw.material->texture()->texels().end(),texels.begin(),texels.end()));
    }
    CHECK(scalar_draws==3);
    CHECK(frame.value().draws[0].material->sampling()==render::TextureSampling::LinearClampMips);
    CHECK(std::equal(frame.value().draws[0].material->texture()->texels().begin(),frame.value().draws[0].material->texture()->texels().end(),texels.begin(),texels.end()));
  }
}
void legacy_color_scalar_package_rejection(){
  namespace sf=assets::surface;
  FixturePackage f;auto valid=f.pack();auto retained=d::prepare_package(valid,*f.doc->schemas());CHECK(retained);
  const auto generation=f.doc->generation();const auto original=f.doc->snapshot().value();
  auto separate=std::move(d::Document::create()).value();auto object=separate->add_box();CHECK(object);
  sf::Stamp stamp;stamp.representation=sf::Representation::Cooked;stamp.target=sf::portable_cpu_target;
  const auto label=std::string_view("scalar-package-rejection");stamp.source=stamp.settings=stamp.importer=stamp.cooker=assets::digest(std::as_bytes(std::span(label)));
  const std::array<std::byte,4> texels{std::byte{31},std::byte{127},std::byte{223},std::byte{255}};
  auto texture=sf::encode_texture_scalar_mips({130,1},stamp,1,1,sf::Encoding::LinearData,texels);CHECK(texture);
  const sf::Reference pin{{130,1},assets::digest(texture.value())};
  for(const auto sampling:{sf::Sampling::LinearClampMip0,sf::Sampling::LinearClampMips}){
    sf::Material material;material.sampling=sampling;material.base_color_texture=pin;
    auto bound_stamp=stamp;bound_stamp.dependencies=sf::dependency_digest(pin);
    auto bound=sf::encode_material({131,1},bound_stamp,material);CHECK(bound);
    const d::MaterialReference ref{{131,1},assets::digest(bound.value())};CHECK(separate->assign_material(object.value(),ref));
    FakeFiles files;files.data[assets::hex(pin.revision)+".rasset"]=texture.value();files.data[assets::hex(ref.revision)+".rasset"]=bound.value();
    CHECK(separate->cook(files,"scalar.rscene"));CHECK_ERROR(d::pack_scene(files,"scalar.rscene",{132,1},*f.doc->schemas()),ErrorCode::InvalidInput);
    std::array<p::Input,3> inputs{{{{132,1},p::Kind::Scene,files.data["scalar.rscene"],{{ref.asset,ref.revision}},1},
      {ref.asset,p::Kind::Material,bound.value(),{{pin.asset,pin.revision}},sampling==sf::Sampling::LinearClampMips?2u:1u},
      {pin.asset,p::Kind::Texture,texture.value(),{},3}}};
    auto package=p::encode(inputs);CHECK(package);CHECK_ERROR(d::prepare_package(package.value(),*f.doc->schemas()),ErrorCode::InvalidInput);
    CHECK(f.doc->generation()==generation&&d::material_references(f.doc->snapshot().value()).value()==d::material_references(original).value());
    CHECK(retained.value().meshes->entries().size()==2&&retained.value().meshes->materials().empty());
  }
  // The package container admits supported opaque kind/schema tags; only native
  // desktop decoding establishes that the bytes really describe a Material.
  CHECK(separate->assign_material(object.value(),d::MaterialReference{pin.asset,pin.revision}));
  FakeFiles confused;confused.data[assets::hex(pin.revision)+".rasset"]=texture.value();CHECK(separate->cook(confused,"confused.rscene"));
  std::array<p::Input,2> opaque{{{{132,2},p::Kind::Scene,confused.data["confused.rscene"],{{pin.asset,pin.revision}},1},
      {pin.asset,p::Kind::Material,texture.value(),{},3}}};
  auto container=p::encode(opaque);CHECK(container&&p::validate(container.value()));
  CHECK(!d::prepare_package(container.value(),*f.doc->schemas()));CHECK(!d::pack_scene(confused,"confused.rscene",{132,2},*f.doc->schemas()));
  CHECK(f.doc->generation()==generation&&retained.value().meshes->materials().empty());
}
void lighting_package_roundtrip(){
  FixturePackage f;d::LightingSettings s;s.light=d::DirectionalLightSettings{{.1,.9,.3},2500,true};s.output=d::RenderOutputSettings{};s.output->exposure_stops=1.5;
  CHECK(f.doc->set_lighting(s,f.doc->generation()));CHECK(f.doc->cook(*f.files,"scene.rscene"));
  auto bytes=f.pack();auto prepared=d::prepare_package(bytes,*f.doc->schemas());CHECK(prepared);
  auto lit=d::lighting_settings(prepared.value().scene);CHECK(lit&&lit.value().light==s.light&&lit.value().output==s.output);
  auto target=std::move(d::Document::create()).value();CHECK(target->install(prepared.value().scene,target->persistence_stamp()));
  CHECK(target->lighting().value().light==s.light&&target->lighting().value().output==s.output);
  auto frame=d::scene_frame(*target,prepared.value().meshes.get(),{});
  CHECK(frame&&frame.value().lighting_profile==render::LightingProfile::PhysicalDiffuseV1&&frame.value().physical.exposure_stops==1.5&&frame.value().draws.size()==4);
}
d::PersistenceCompletion finish(d::PersistenceTask& task,d::Document& doc){
  for(int n=0;n<5000;++n){auto r=task.poll(doc);CHECK(r);if(r.value())return std::move(*r.value());std::this_thread::sleep_for(1ms);}CHECK(false);std::abort();
}
void worker_ownership(){
  FixturePackage f;auto bytes=f.pack();auto doc=std::move(d::Document::create()).value();
  auto files=std::make_unique<FakeFiles>();files->data["content.rpak"]=bytes;auto* raw=files.get();
  auto gate=std::make_shared<Gate>();files->gate=gate;files->gated="content.rpak";
  auto candidate=std::make_shared<std::shared_ptr<const d::MeshSet>>();
  const auto owner=std::this_thread::get_id();std::atomic<bool> worker{};
  d::PersistenceTask task(std::move(files));
  auto decode=[candidate,&worker,owner](platform::Files& source,std::string_view path,const scene::Schemas& schemas)->Result<scene::Document>{
    worker=std::this_thread::get_id()!=owner;auto ready=d::read_package(source,path,schemas);if(!ready)return ready.error();*candidate=std::move(ready.value().meshes);return std::move(ready.value().scene);
  };
  CHECK(!task.start(*doc,d::PersistenceKind::PackageLoad,"content.rpak"));
  const auto generation=doc->generation();CHECK(task.start(*doc,d::PersistenceKind::PackageLoad,"content.rpak",{},decode));gate->wait();
  CHECK_ERROR(task.start(*doc,d::PersistenceKind::PackageLoad,"content.rpak",{},decode),ErrorCode::Busy);
  CHECK(task.cancel_load());CHECK(!task.poll(*doc).value());CHECK(task.busy()&&doc->generation()==generation);
  gate->release();auto cancelled=finish(task,*doc);CHECK_ERROR(cancelled.result,ErrorCode::Cancelled);CHECK(worker&&doc->generation()==generation);
  std::weak_ptr<platform::RootedFiles> retained=(*candidate)->files();CHECK(!retained.expired());candidate->reset();CHECK(retained.expired());
  raw->gate.reset();CHECK(task.start(*doc,d::PersistenceKind::PackageLoad,"content.rpak",{},decode));CHECK(doc->add_box());const auto edited=doc->generation();
  auto stale=finish(task,*doc);CHECK_ERROR(stale.result,ErrorCode::StaleHandle);CHECK(doc->generation()==edited);candidate->reset();CHECK(doc->undo());CHECK(doc->redo());
  CHECK(task.start(*doc,d::PersistenceKind::PackageLoad,"content.rpak",{},decode));auto loaded=finish(task,*doc);CHECK(loaded.result&&*candidate);CHECK(d::mesh_references(doc->snapshot().value()).value().size()==3);
  auto owned_frame=d::scene_frame(*doc,candidate->get(),{});CHECK(owned_frame);const auto live_generation=doc->generation();auto old=*candidate;
  raw->data["content.rpak"].back()^=std::byte{1};CHECK(task.start(*doc,d::PersistenceKind::PackageLoad,"content.rpak",{},decode));auto failed=finish(task,*doc);
  CHECK(!failed.result&&doc->generation()==live_generation&&*candidate==old&&owned_frame.value().draws[1].geometry);
}
}
int main(){roundtrip_and_lifetime();closure_and_preservation();publication();failures_and_decode();mixed_surface_schemas();legacy_color_scalar_package_rejection();lighting_package_roundtrip();worker_ownership();std::puts("desktop package exact closure, deterministic publication, preservation and ownership passed");}
