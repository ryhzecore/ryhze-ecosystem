#include "desktop_mesh_checkpoint.hpp"
#include "content_mesh_checkpoint.hpp"
#include "mesh_pipeline.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <stdexcept>

namespace {
namespace d=race::desktop;
namespace c=race::mesh_checkpoint;
namespace m=race::assets::mesh;
const char* phase="startup";std::size_t fault_step{};
constexpr std::string_view source_a="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view source_b="v 0 0 0\nv 3 0 0\nv 0 2 0\nf 1 2 3\n";
constexpr std::string_view source_uv_a="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
constexpr std::string_view source_uv_b="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 1 0\nvt 0 0\nvt 1 1\nf 1/1 2/2 3/3\n";
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
std::vector<std::byte> product(AssetId id,std::string_view text) {
  auto imported=mesh_pipeline::import_obj(id,bytes(text));CHECK(imported);
  auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);return std::move(cooked).value();
}
d::MeshReference put(FakeFiles& files,const std::vector<std::byte>& value) {
  auto view=m::validate(value);CHECK(view);
  files.data[hex(view.value().revision())+".rasset"]=value;
  return {view.value().id(),view.value().revision()};
}
struct Fixture {
  std::shared_ptr<FakeFiles> products=std::make_shared<FakeFiles>();
  std::shared_ptr<FakeFiles> recipes=std::make_shared<FakeFiles>();
  std::shared_ptr<FakeFiles> checkpoints=std::make_shared<FakeFiles>();
  std::unique_ptr<d::Document> doc;
  std::shared_ptr<const d::MeshSet> current;
  std::vector<std::byte> a,b;
  d::MeshReference old_ref,new_ref;
  PersistentId selected,other;
  std::string checkpoint_leaf;
  void project_profile(mesh_pipeline::Profile profile){
    std::array<mesh_project::Reference,2> refs;
    for(std::size_t n=0;n<refs.size();++n) {
      const auto leaf="recipe-"+std::to_string(n)+".json";
      auto raw=mesh_recipe::encode({aid(n+1),"source.obj",n?mesh_pipeline::Profile::Geometry:profile});CHECK(raw);
      recipes->data[leaf]={bytes(raw.value()).begin(),bytes(raw.value()).end()};
      refs[n]={aid(n+1),leaf,digest(bytes(raw.value()))};
    }
    auto index=mesh_project::encode(refs);CHECK(index);
    recipes->data["project.raceproject"]={std::span<const std::byte>(index.value()).begin(),std::span<const std::byte>(index.value()).end()};
  }
  void typed_project(){
    auto original=content_project::load(*recipes,"project.raceproject");CHECK(original);
    std::vector<content_project::Reference> refs;
    for(const auto& entry:original.value().entries())refs.push_back(entry.reference);
    for(unsigned n=0;n<2;++n){
      const auto leaf=n?"material.json":"texture.json";
      auto raw=surface_recipe::encode({aid(n+3),n?"material-source.json":"texture.bmp",
          n?surface_pipeline::Profile::Material:surface_pipeline::Profile::BmpSrgbMips});CHECK(raw);
      recipes->data[leaf]={bytes(raw.value()).begin(),bytes(raw.value()).end()};
      refs.push_back({aid(n+3),n?content_project::Kind::Material:content_project::Kind::Texture,leaf,digest(bytes(raw.value()))});
    }
    auto index=content_project::encode_binary(refs);CHECK(index);const auto& raw=index.value();
    recipes->data["project.raceproject"]=raw;checkpoint_leaf=manifest(b);
    recipes->reset_reads();products->reset_reads();checkpoints->reset_reads();
  }
  explicit Fixture(mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry)
      :a(product(aid(1),profile==mesh_pipeline::Profile::GeometryUv?source_uv_a:source_a)),
       b(product(aid(1),profile==mesh_pipeline::Profile::GeometryUv?source_uv_b:source_b)) {
    auto made=d::Document::create();CHECK(made);doc=std::move(made).value();
    old_ref=put(*products,a);new_ref=put(*products,b);project_profile(profile);
    auto first=doc->add_mesh(old_ref),second=doc->add_mesh(old_ref);CHECK(first&&second);
    selected=first.value();other=second.value();
    auto selected_row=row(selected);selected_row.local.translation={7,8,9};selected_row.color={.1,.2,.3};
    CHECK(doc->edit(selected_row,doc->generation()));
    std::array instances{old_ref,old_ref};auto set=d::MeshSet::prepare(products,instances);CHECK(set);current=std::move(set).value();
    checkpoint_leaf=manifest(b);products->reset_reads();
  }
  d::ObjectRow row(PersistentId id) const {
    auto rows=doc->objects();CHECK(rows);
    for(const auto& item:rows.value())if(item.id==id)return item;
    CHECK(false);return {};
  }
  void repin_index() {
    auto prior=content_project::load(*recipes,"project.raceproject");CHECK(prior);
    std::vector<content_project::Reference> refs;
    for(const auto& row:prior.value().entries())refs.push_back(row.reference);
    recipes->data["alias.json"]=recipes->data.at(refs.back().recipe);refs.back().recipe="alias.json";
    auto changed=content_project::encode_binary(refs);CHECK(changed);recipes->data["project.raceproject"]=changed.value();
  }
  std::string manifest(const std::vector<std::byte>& value) {
    auto view=m::validate(value);CHECK(view);put(*products,value);
    auto project=content_project::load(*recipes,"project.raceproject");CHECK(project);
    auto recipe=project.value().find(view.value().id());CHECK(recipe);
    const c::Definition definition{view.value().id(),project.value().revision(),
        recipe.value()->reference.revision,view.value().stamp().source,view.value().revision()};
    auto encoded=c::encode(definition);CHECK(encoded);auto leaf=hex(digest(bytes(encoded.value())))+".json";
    checkpoints->data[leaf]={bytes(encoded.value()).begin(),bytes(encoded.value()).end()};return leaf;
  }
  d::MeshCheckpointRequest request() const {return {"recipes","project.raceproject","checkpoints",checkpoint_leaf};}
  d::MeshCheckpointTask::RootFactory factory() const {
    return [recipes=recipes,checkpoints=checkpoints](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>> {
      if(name=="recipes")return std::shared_ptr<platform::RootedFiles>(recipes);
      if(name=="checkpoints")return std::shared_ptr<platform::RootedFiles>(checkpoints);
      return fail(ErrorCode::NotFound,"fixture root missing");
    };
  }
  std::shared_ptr<Gate> block() {
    auto gate=std::make_shared<Gate>();products->gate=gate;products->gated=hex(new_ref.revision)+".rasset";return gate;
  }
};
d::MeshCheckpointCompletion finish(d::MeshCheckpointTask& task,d::Document& doc,
    std::shared_ptr<const d::MeshSet>& meshes,PersistentId selection) {
  const auto deadline=std::chrono::steady_clock::now()+10s;
  while(std::chrono::steady_clock::now()<deadline) {
    auto polled=task.poll(doc,meshes,selection);CHECK(polled);
    if(polled.value())return std::move(*polled.value());
    std::this_thread::sleep_for(1ms);
  }
  std::fprintf(stderr,"checkpoint completion timeout phase=%s step=%zu busy=%d\n",phase,fault_step,int(task.busy()));
  CHECK(false);return {};
}
void selected_only_and_history() {
  Fixture f;auto prior=f.current;const auto selected_before=f.row(f.selected),other_before=f.row(f.other);
  const auto generation=f.doc->generation();auto old_packet=prior->find(f.old_ref).value()->geometry;
  d::MeshCheckpointTask task(f.factory());auto admitted=task.start(*f.doc,f.current,f.selected,f.request());CHECK(admitted&&task.busy());
  auto done=finish(task,*f.doc,f.current,f.selected);CHECK(done.result&&done.changed&&!task.busy());
  CHECK(done.request==admitted.value()&&done.selected==f.selected&&done.previous==f.old_ref&&done.replacement==f.new_ref);
  CHECK(done.product_bytes==f.b.size()&&done.snapshot_ms>=0&&done.load_ms>=0&&done.prepare_ms>=0&&done.commit_ms>=0);
  CHECK(f.products->read_count()==1); // No extension reread and no old-product reads.
  CHECK(f.doc->generation()>generation&&f.current!=prior&&f.current->entries().size()==2);
  const auto selected_after=f.row(f.selected),other_after=f.row(f.other);
  CHECK(selected_after.mesh==f.new_ref&&other_after.mesh==f.old_ref);
  CHECK(selected_after.handle==selected_before.handle&&selected_after.local==selected_before.local&&selected_after.color==selected_before.color);
  CHECK(other_after.handle==other_before.handle&&other_after.local==other_before.local&&other_after.color==other_before.color);
  CHECK(f.current->find(f.old_ref).value()->geometry==old_packet);
  CHECK(d::scene_frame(*f.doc,f.current.get(),{}));
  CHECK(f.doc->undo()&&f.row(f.selected).mesh==f.old_ref);CHECK(d::scene_frame(*f.doc,f.current.get(),{}));
  CHECK(f.doc->redo()&&f.row(f.selected).mesh==f.new_ref);
  CHECK(f.doc->undo());CHECK(f.doc->undo());CHECK(f.row(f.selected).local.translation==scene::Vec3{});
  CHECK(f.doc->redo());CHECK(f.doc->redo());CHECK(f.row(f.selected).local==selected_before.local);
  CHECK(f.row(f.other).mesh==f.old_ref&&old_packet->vertices().size()==3);
}
void typed_project_adoption(){
  for(auto profile:{mesh_pipeline::Profile::Geometry,mesh_pipeline::Profile::GeometryUv}){
    Fixture f(profile);f.typed_project();const auto exact=digest(f.recipes->data["project.raceproject"]);
    auto definition=c::decode(f.checkpoints->data[f.checkpoint_leaf]);CHECK(definition&&definition.value().index_revision==exact);
    auto request=f.request();request.expected_project_revision=exact;
    const auto before=f.row(f.selected),other=f.row(f.other);auto prior=f.current;
    const auto retained=prior->find(f.old_ref).value()->geometry;
    d::MeshCheckpointTask task(f.factory());CHECK(task.start(*f.doc,f.current,f.selected,request));
    const auto done=finish(task,*f.doc,f.current,f.selected);CHECK(done.result&&done.changed);
    CHECK(f.recipes->read_count()==5&&f.checkpoints->read_count()==1&&f.products->read_count()==1);
    CHECK(f.row(f.selected).mesh==f.new_ref&&f.row(f.selected).handle==before.handle&&f.row(f.selected).local==before.local);
    CHECK(f.row(f.other).mesh==other.mesh&&f.current->find(f.old_ref).value()->geometry==retained);
    CHECK(f.doc->undo()&&f.row(f.selected).mesh==f.old_ref);CHECK(f.doc->redo()&&f.row(f.selected).mesh==f.new_ref);
  }
  for(auto leaf:{"texture.json","material.json","project.raceproject"}){
    Fixture f;f.typed_project();auto request=f.request();request.expected_project_revision=digest(f.recipes->data["project.raceproject"]);
    if(std::string_view{leaf}=="project.raceproject")f.repin_index();else f.recipes->data[leaf].push_back(std::byte{' '});
    const auto prior=f.current;const auto generation=f.doc->generation();
    d::MeshCheckpointTask task(f.factory());CHECK(task.start(*f.doc,f.current,f.selected,request));
    const auto done=finish(task,*f.doc,f.current,f.selected);CHECK_ERROR(done.result,ErrorCode::InvalidDependency);
    CHECK(!done.changed&&f.current==prior&&f.doc->generation()==generation&&f.row(f.selected).mesh==f.old_ref);
    CHECK(f.checkpoints->read_count()==0&&f.products->read_count()==0);
  }
}
// Deliberate negative fixture: structurally valid geometry with a contradictory
// first-party profile. Never use these bytes as successful UV provenance.
std::vector<std::byte> mismatched_profile(const std::vector<std::byte>& bytes,bool has_uv,m::Stamp stamp){
  auto view=m::validate(bytes);CHECK(view);std::array<m::Vertex,3> vertices;
  for(std::size_t i=0;i<vertices.size();++i){vertices[i]=view.value().vertex(i).value();if(!has_uv)vertices[i].uv={};}
  const std::array<std::uint32_t,3> indices{0,1,2};
  auto encoded=m::encode(view.value().id(),stamp,vertices,indices,{},has_uv);CHECK(encoded);return std::move(encoded).value();
}
void checkpoint_profile_binding(){
  for(unsigned mode=0;mode<4;++mode){
    Fixture f(mode<2?mesh_pipeline::Profile::GeometryUv:mesh_pipeline::Profile::Geometry);
    const auto genuine=product(aid(1),source_uv_b);auto view=m::validate(genuine);CHECK(view&&view.value().has_uv());
    const auto legacy=mesh_pipeline::cook_stamp(mesh_pipeline::import_stamp(view.value().stamp().source));
    // UV recipe + legacy stamp; UV recipe + missing UVs; legacy recipe + UV
    // stamp; legacy recipe + UV body with otherwise matching legacy stamp.
    const auto invalid=mode==0?mismatched_profile(genuine,true,legacy):
        mode==1?mismatched_profile(genuine,false,view.value().stamp()):
        mode==2?genuine:mismatched_profile(genuine,true,legacy);
    f.checkpoint_leaf=f.manifest(invalid);f.products->reset_reads();
    const auto before=f.row(f.selected),other=f.row(f.other);const auto generation=f.doc->generation();const auto prior=f.current;
    d::MeshCheckpointTask task(f.factory());CHECK(task.start(*f.doc,f.current,f.selected,f.request()));
    const auto done=finish(task,*f.doc,f.current,f.selected);CHECK_ERROR(done.result,ErrorCode::InvalidDependency);
    CHECK(!done.changed&&f.current==prior&&f.doc->generation()==generation&&f.products->read_count()==1);
    CHECK(f.row(f.selected).mesh==before.mesh&&f.row(f.selected).local==before.local&&f.row(f.other).mesh==other.mesh);
    CHECK(f.doc->undo());CHECK(f.row(f.selected).local.translation==scene::Vec3{});
    CHECK(f.doc->redo());CHECK(f.row(f.selected).local==before.local);
  }
}
void material_binding_admission(){
  namespace sf=assets::surface;
  Fixture f(mesh_pipeline::Profile::GeometryUv);const auto uv_a=f.old_ref,uv_b=f.new_ref;
  auto a=m::validate(f.a),b=m::validate(f.b);CHECK(a&&b&&a.value().has_uv()&&b.value().has_uv());
  CHECK(a.value().stamp().settings!=mesh_pipeline::import_stamp(a.value().stamp().source).settings);
  CHECK(a.value().bounds()==b.value().bounds()&&uv_a.revision!=uv_b.revision);
  for(std::size_t i=0;i<a.value().vertex_count();++i){CHECK(a.value().vertex(i).value().position==b.value().vertex(i).value().position);CHECK(a.value().vertex(i).value().normal==b.value().vertex(i).value().normal);}
  CHECK(a.value().vertex(0).value().uv!=b.value().vertex(0).value().uv);
  sf::Stamp stamp;stamp.representation=sf::Representation::Cooked;stamp.target=sf::portable_cpu_target;
  stamp.source=stamp.settings=stamp.importer=stamp.cooker=digest(bytes("checkpoint-material"));
  const std::array<std::byte,4> pixels{std::byte{255},std::byte{128},std::byte{},std::byte{255}};
  auto texture=sf::encode_texture({99,1},stamp,1,1,sf::Encoding::SrgbColor,pixels);CHECK(texture);auto texture_view=sf::validate_texture(texture.value());CHECK(texture_view);
  f.products->data[hex(texture_view.value().revision())+".rasset"]=texture.value();
  sf::Material mat;mat.base_color_texture=sf::Reference{texture_view.value().id(),texture_view.value().revision()};stamp.dependencies=sf::dependency_digest(mat.base_color_texture);
  auto material=sf::encode_material({99,2},stamp,mat);CHECK(material);auto material_view=sf::validate_material(material.value());CHECK(material_view);
  const d::MaterialReference material_ref{material_view.value().id(),material_view.value().revision()};
  f.products->data[hex(material_ref.revision)+".rasset"]=material.value();CHECK(f.doc->assign_material(f.selected,material_ref));
  const std::array refs{uv_a,f.old_ref};auto prepared=d::MeshSet::prepare(f.products,refs,{},std::span(&material_ref,1));CHECK(prepared);f.current=std::move(prepared).value();
  CHECK(f.current->validate_scene(f.doc->snapshot().value()));CHECK(f.doc->cook(*f.products,"before.rscene"));
  const auto generation=f.doc->generation();const auto prior=f.current;const auto before=f.row(f.selected),other=f.row(f.other);
  // A separately valid legacy recipe/checkpoint still cannot drop UVs from an
  // object whose retained material requires them. Profile checks pass first.
  f.project_profile(mesh_pipeline::Profile::Geometry);f.checkpoint_leaf=f.manifest(product(aid(1),source_b));
  d::MeshCheckpointTask task(f.factory());CHECK(task.start(*f.doc,f.current,f.selected,f.request()));
  auto rejected=finish(task,*f.doc,f.current,f.selected);CHECK_ERROR(rejected.result,ErrorCode::InvalidInput);
  CHECK(!rejected.changed&&!task.busy()&&f.current==prior&&f.doc->generation()==generation);
  CHECK(f.row(f.selected).mesh==uv_a&&f.row(f.selected).material==material_ref&&f.row(f.selected).handle==before.handle);
  CHECK(f.row(f.selected).local==before.local&&f.row(f.selected).color==before.color&&f.row(f.other).mesh==other.mesh);
  CHECK(f.doc->cook(*f.products,"after.rscene"));CHECK(f.products->data["before.rscene"]==f.products->data["after.rscene"]);
  CHECK(d::scene_frame(*f.doc,f.current.get(),{}));CHECK(f.doc->undo());CHECK(!f.row(f.selected).material);CHECK(f.doc->redo());CHECK(f.row(f.selected).material==material_ref);
  f.project_profile(mesh_pipeline::Profile::GeometryUv);f.checkpoint_leaf=f.manifest(f.b);
  const auto retained_material=prior->find_material(material_ref).value()->material;
  const auto retained_texture=retained_material->texture();const auto retained_geometry=prior->find(uv_a).value()->geometry;
  // Adoption has only the new product on disk. Material, texture and old mesh
  // revisions must come from the immutable prepared cache, including Undo.
  f.products->data.erase(hex(uv_a.revision)+".rasset");
  f.products->data.erase(hex(material_ref.revision)+".rasset");
  f.products->data.erase(hex(texture_view.value().revision())+".rasset");f.products->reset_reads();
  CHECK(task.start(*f.doc,f.current,f.selected,f.request()));auto accepted=finish(task,*f.doc,f.current,f.selected);CHECK(accepted.result&&accepted.changed);
  CHECK(f.products->read_count()==1&&f.current->find(uv_a).value()->geometry==retained_geometry);
  CHECK(f.row(f.selected).mesh==uv_b&&f.row(f.selected).material==material_ref&&f.row(f.other).mesh==f.old_ref);
  CHECK(f.current->find_material(material_ref).value()->material==retained_material);
  CHECK(f.current->find_material(material_ref).value()->material->texture()==retained_texture);
  CHECK(d::scene_frame(*f.doc,f.current.get(),{}));CHECK(f.doc->undo());CHECK(f.row(f.selected).mesh==uv_a&&f.row(f.selected).material==material_ref);
  CHECK(d::scene_frame(*f.doc,f.current.get(),{}));CHECK(f.doc->redo());CHECK(f.row(f.selected).mesh==uv_b);
  CHECK(f.products->read_count()==1&&f.row(f.selected).local==before.local&&f.row(f.selected).color==before.color);
}
void identical_noop() {
  Fixture f;f.checkpoint_leaf=f.manifest(f.a);f.products->reset_reads();
  const auto generation=f.doc->generation();const auto prior=f.current;
  d::MeshCheckpointTask task(f.factory());CHECK(task.start(*f.doc,f.current,f.selected,f.request()));
  auto done=finish(task,*f.doc,f.current,f.selected);CHECK(done.result&&!done.changed&&done.previous==done.replacement);
  CHECK(f.doc->generation()==generation&&f.current==prior&&f.products->read_count()==1);
  CHECK(f.doc->undo());CHECK(f.row(f.selected).local.translation==scene::Vec3{}); // Existing top history remains.
}
void displayed_project_pins() {
  for(unsigned mode=0;mode<3;++mode) {
    Fixture f;auto names=f.request();CHECK(!names.expected_project_revision);
    const auto displayed=digest(f.recipes->data.at("project.raceproject"));names.expected_project_revision=displayed;
    if(mode==1)(*names.expected_project_revision)[0]^=std::byte{1};
    if(mode==2) {
      // A valid index can use a different canonical recipe leaf while keeping
      // exact recipe contents. Even a new matching checkpoint cannot bypass
      // the displayed project revision pin.
      f.repin_index();
      CHECK(digest(f.recipes->data.at("project.raceproject"))!=displayed);
      names.checkpoint_leaf=f.manifest(f.b);
    }
    f.recipes->reset_reads();f.checkpoints->reset_reads();f.products->reset_reads();
    std::atomic<unsigned> checkpoint_roots{};auto base=f.factory();
    d::MeshCheckpointTask task([base,&checkpoint_roots](std::string_view name){
      if(name=="checkpoints")++checkpoint_roots;return base(name);
    });
    const auto generation=f.doc->generation();const auto prior=f.current;
    const auto selected=f.row(f.selected),other=f.row(f.other);
    CHECK(task.start(*f.doc,f.current,f.selected,names));auto done=finish(task,*f.doc,f.current,f.selected);
    CHECK(f.recipes->read_count()==3); // One index and its complete two-recipe closure.
    if(mode==0) {
      CHECK(done.result&&done.changed&&f.current!=prior&&f.row(f.selected).mesh==f.new_ref);
      CHECK(checkpoint_roots==1&&f.checkpoints->read_count()==1&&f.products->read_count()==1);
      CHECK(f.doc->undo());CHECK(f.row(f.selected).mesh==f.old_ref);
      CHECK(f.row(f.selected).local==selected.local&&f.row(f.other).mesh==other.mesh);
    }else {
      CHECK_ERROR(done.result,ErrorCode::InvalidDependency);CHECK(!done.changed&&!task.busy());
      CHECK(done.previous==f.old_ref&&done.replacement==f.old_ref&&done.product_bytes==0&&done.prepare_ms==0);
      CHECK(checkpoint_roots==0&&f.checkpoints->read_count()==0&&f.products->read_count()==0);
      CHECK(f.doc->generation()==generation&&f.current==prior);
      CHECK(f.row(f.selected).handle==selected.handle&&f.row(f.selected).mesh==selected.mesh&&f.row(f.selected).local==selected.local&&f.row(f.selected).color==selected.color);
      CHECK(f.row(f.other).handle==other.handle&&f.row(f.other).mesh==other.mesh&&f.row(f.other).local==other.local&&f.row(f.other).color==other.color);
      CHECK(f.doc->undo());CHECK(f.row(f.selected).local.translation==scene::Vec3{});
      CHECK(f.doc->redo());CHECK(f.row(f.selected).local==selected.local&&f.row(f.selected).mesh==selected.mesh);
      // Explicitly unpinned legacy admission still works after a rejected pin.
      names.expected_project_revision.reset();f.recipes->reset_reads();
      CHECK(task.start(*f.doc,f.current,f.selected,names));done=finish(task,*f.doc,f.current,f.selected);
      CHECK(done.result&&done.changed&&f.row(f.selected).mesh==f.new_ref);
      CHECK(f.recipes->read_count()==3&&checkpoint_roots==1&&f.checkpoints->read_count()==1&&f.products->read_count()==1);
    }
  }
}
void closure_failures() {
  for(unsigned mode=0;mode<4;++mode) {
    Fixture f;const auto generation=f.doc->generation();const auto prior=f.current;
    if(mode==0)f.checkpoint_leaf=f.manifest(product(aid(2),source_b));
    if(mode==1)f.recipes->data["recipe-1.json"].push_back(std::byte{' '}); // Unselected closure pin also matters.
    if(mode==2)f.checkpoints->data[f.checkpoint_leaf].back()^=std::byte{1};
    if(mode==3)f.products->data[hex(f.new_ref.revision)+".rasset"].back()^=std::byte{1};
    d::MeshCheckpointTask task(f.factory());CHECK(task.start(*f.doc,f.current,f.selected,f.request()));
    auto done=finish(task,*f.doc,f.current,f.selected);CHECK(!done.result&&!done.changed);
    CHECK(f.doc->generation()==generation&&f.current==prior&&f.row(f.selected).mesh==f.old_ref);
  }
  Fixture f;auto view=m::validate(f.b);CHECK(view);
  std::array<m::Vertex,3> vertices;for(std::size_t n=0;n<vertices.size();++n)vertices[n]=view.value().vertex(n).value();
  std::vector<std::uint32_t> indices(render::viewport_mesh_frame_indices);
  for(std::size_t n=0;n<indices.size();++n)indices[n]=static_cast<std::uint32_t>(n%3);
  auto huge=m::encode(aid(1),view.value().stamp(),vertices,indices);CHECK(huge);f.checkpoint_leaf=f.manifest(huge.value());
  auto prior=f.current;const auto generation=f.doc->generation();d::MeshCheckpointTask task(f.factory());
  CHECK(task.start(*f.doc,f.current,f.selected,f.request()));auto done=finish(task,*f.doc,f.current,f.selected);
  CHECK_ERROR(done.result,ErrorCode::Exhausted);CHECK(!done.changed&&f.current==prior&&f.doc->generation()==generation);
}
void stale_and_cancellation() {
  for(unsigned mode=0;mode<7;++mode) {
    Fixture f;auto gate=f.block();auto prior=f.current;d::MeshCheckpointTask task(f.factory());
    CHECK(task.start(*f.doc,f.current,f.selected,f.request()));gate->wait();
    CHECK_ERROR(task.start(*f.doc,f.current,f.selected,f.request()),ErrorCode::Busy);
    PersistentId selection=f.selected;
    if(mode==0){auto row=f.row(f.selected);row.color={.4,.5,.6};CHECK(f.doc->edit(row,f.doc->generation()));}
    if(mode==1)selection=f.other;
    if(mode==2){CHECK(task.cancel());CHECK(task.cancel());selection=f.other;selection=f.selected;} // Selection ABA.
    if(mode==3){auto empty=d::MeshSet::prepare(f.products,{});CHECK(empty);f.current=std::move(empty).value();}
    if(mode==4){CHECK(f.doc->start_play());CHECK(f.doc->stop_play());}
    if(mode==5){const auto generation=f.doc->generation();CHECK(f.doc->clear_history());CHECK(f.doc->generation()==generation);CHECK(task.cancel());}
    if(mode==6){auto another=d::Document::create();CHECK(another);f.doc=std::move(another).value();}
    const auto before=f.doc->generation();auto expected=f.current;
    auto pending=task.poll(*f.doc,f.current,selection);CHECK(pending&&!pending.value()&&task.busy());
    gate->release();auto done=finish(task,*f.doc,f.current,selection);
    CHECK_ERROR(done.result,mode==2||mode==5?ErrorCode::Cancelled:ErrorCode::StaleHandle);
    CHECK(!done.changed&&!task.busy()&&f.current==expected&&f.doc->generation()==before);
    if(mode!=6)CHECK(f.row(f.selected).mesh==f.old_ref);
  }
  Fixture f;auto gate=f.block();d::MeshCheckpointTask task(f.factory());
  auto first=task.start(*f.doc,f.current,f.selected,f.request());CHECK(first);gate->wait();CHECK(task.cancel());gate->release();
  auto cancelled=finish(task,*f.doc,f.current,f.selected);CHECK_ERROR(cancelled.result,ErrorCode::Cancelled);
  auto second=task.start(*f.doc,f.current,f.selected,f.request());CHECK(second&&second.value()==first.value()+1);
  CHECK(finish(task,*f.doc,f.current,f.selected).changed); // New admission resets sticky cancellation.
}
void owner_and_admission() {
  Fixture f;d::MeshCheckpointTask task(f.factory());
  std::thread foreign([&] {
    CHECK_ERROR(task.start(*f.doc,f.current,f.selected,f.request()),ErrorCode::WrongThread);
    CHECK_ERROR(task.cancel(),ErrorCode::WrongThread);
    CHECK_ERROR(task.poll(*f.doc,f.current,f.selected),ErrorCode::WrongThread);
  });foreign.join();CHECK(!task.busy());CHECK_ERROR(task.cancel(),ErrorCode::NotFound);
  for(unsigned mode=0;mode<5;++mode) {
    auto names=f.request();
    if(mode==0)names.recipe_root.assign(4097,'a');
    if(mode==1)names.checkpoint_root.push_back('\0');
    if(mode==2)names.project_leaf="../project.raceproject";
    if(mode==3)names.checkpoint_leaf="checkpoint.json";
    if(mode==4)names.checkpoint_leaf[0]='G';
    CHECK_ERROR(task.start(*f.doc,f.current,f.selected,std::move(names)),ErrorCode::InvalidInput);CHECK(!task.busy());
  }
  for(unsigned mode=0;mode<4;++mode) {
    auto names=f.request();
    if(mode==0)names.recipe_root.reserve(8192);
    if(mode==1)names.checkpoint_root.reserve(8192);
    if(mode==2)names.project_leaf.reserve(1024);
    if(mode==3)names.checkpoint_leaf.reserve(1024);
    CHECK_ERROR(task.start(*f.doc,f.current,f.selected,std::move(names)),ErrorCode::Exhausted);CHECK(!task.busy());
  }
  CHECK_ERROR(task.start(*f.doc,f.current,f.row(f.doc->objects().value().front().id).id,f.request()),ErrorCode::InvalidInput);
  CHECK_ERROR(task.start(*f.doc,f.current,{99,99},f.request()),ErrorCode::NotFound);
  CHECK(f.doc->start_play());CHECK_ERROR(task.start(*f.doc,f.current,f.selected,f.request()),ErrorCode::Busy);CHECK(f.doc->stop_play());
  for(unsigned mode=0;mode<3;++mode) {
    d::MeshCheckpointTask broken([mode](std::string_view)->Result<std::shared_ptr<platform::RootedFiles>> {
      if(mode==0)throw std::runtime_error("worker fixture");
      if(mode==1)throw std::bad_alloc();
      return std::shared_ptr<platform::RootedFiles>{};
    });
    const auto generation=f.doc->generation();const auto prior=f.current;
    CHECK(broken.start(*f.doc,f.current,f.selected,f.request()));auto done=finish(broken,*f.doc,f.current,f.selected);
    CHECK_ERROR(done.result,mode==0?ErrorCode::PlatformFailure:mode==1?ErrorCode::Exhausted:ErrorCode::InvalidInput);
    CHECK(!done.changed&&f.current==prior&&f.doc->generation()==generation);
  }
}
void destructor_retains_blocked_owners() {
  Fixture f;auto gate=f.block();std::weak_ptr<FakeFiles> products=f.products,recipes=f.recipes,checkpoints=f.checkpoints;
  auto task=std::make_unique<d::MeshCheckpointTask>(f.factory());CHECK(task->start(*f.doc,f.current,f.selected,f.request()));gate->wait();
  f.current.reset();f.products.reset();f.recipes.reset();f.checkpoints.reset();f.doc.reset();
  CHECK(!products.expired()&&!recipes.expired()&&!checkpoints.expired());
  std::atomic<bool> destroyed{};
  std::thread release([&]{std::this_thread::sleep_for(5ms);CHECK(!destroyed.load());gate->release();});
  task.reset();destroyed=true;release.join();
  CHECK(products.expired()&&recipes.expired()&&checkpoints.expired());
}
struct WorkerFaultRoot final : platform::RootedFiles {
  std::shared_ptr<platform::RootedFiles> inner;
  explicit WorkerFaultRoot(std::shared_ptr<platform::RootedFiles> value):inner(std::move(value)){}
  ~WorkerFaultRoot() override { d::profile::testing::fail_next_allocation_attempts(0); }
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override {
    return inner->read(leaf,limit);
  }
  Result<void> publish_immutable(std::string_view leaf,std::span<const std::byte> value) override {
    return inner->publish_immutable(leaf,value);
  }
  Result<void> replace(std::string_view leaf,std::span<const std::byte> value,
      platform::Durability durability) override {
    return inner->replace(leaf,value,durability);
  }
};

void allocation_failures() {
  std::size_t admission_failures{},commit_failures{};
  for(std::size_t step=0;step<64;++step) {
    phase="allocation-admission";fault_step=step;
    Fixture f;d::MeshCheckpointTask task(f.factory());auto names=f.request();const auto generation=f.doc->generation();auto prior=f.current;
    d::profile::testing::fail_allocation_after(step);
    auto admitted=task.start(*f.doc,f.current,f.selected,std::move(names));
    d::profile::testing::fail_next_allocation_attempts(0);
    if(admitted){auto done=finish(task,*f.doc,f.current,f.selected);CHECK(done.result&&done.changed);break;}
    CHECK_ERROR(admitted,ErrorCode::Exhausted);CHECK(!task.busy()&&f.doc->generation()==generation&&f.current==prior);
    CHECK(task.start(*f.doc,f.current,f.selected,f.request()));CHECK(finish(task,*f.doc,f.current,f.selected).changed);++admission_failures;
  }
  CHECK(admission_failures>0&&admission_failures<64);
  for(std::size_t step=0;step<64;++step) {
    phase="allocation-commit";fault_step=step;
    Fixture f;d::MeshCheckpointTask task(f.factory());
    CHECK(task.start(*f.doc,f.current,f.selected,f.request()));
    const auto generation=f.doc->generation();auto prior=f.current;
    // Failure injection is thread-local, so it affects only completion/edit.
    d::profile::testing::fail_allocation_after(step);
    auto done=finish(task,*f.doc,f.current,f.selected);
    d::profile::testing::fail_next_allocation_attempts(0);
    if(done.result){CHECK(done.changed);break;}
    CHECK_ERROR(done.result,ErrorCode::Exhausted);CHECK(!done.changed&&f.doc->generation()==generation&&f.current==prior);
    CHECK(task.start(*f.doc,f.current,f.selected,f.request()));CHECK(finish(task,*f.doc,f.current,f.selected).changed);++commit_failures;
  }
  CHECK(commit_failures>0&&commit_failures<64);
  // A factory arms the worker's thread-local allocation fault immediately after
  // roots/project are admitted. Each failed preparation leaves live state intact.
  std::size_t worker_failures{};
  for(std::size_t step=0;step<64;++step) {
    phase="allocation-worker";fault_step=step;
    Fixture f;auto base=f.factory();
    d::MeshCheckpointTask task([base,step](std::string_view name) mutable
        ->Result<std::shared_ptr<platform::RootedFiles>> {
      auto result=base(name);
      if(!result||name!="checkpoints")return result;
      // Allocate the guard BEFORE arming; it is owned only by the worker's
      // local checkpoint-root result and dies on every return/unwind path.
      auto scoped=std::make_shared<WorkerFaultRoot>(std::move(result).value());
      d::profile::testing::fail_allocation_after(step);
      return std::shared_ptr<platform::RootedFiles>(std::move(scoped));
    });
    const auto generation=f.doc->generation();auto prior=f.current;
    CHECK(task.start(*f.doc,f.current,f.selected,f.request()));auto done=finish(task,*f.doc,f.current,f.selected);
    if(done.result){CHECK(done.changed);break;}
    CHECK_ERROR(done.result,ErrorCode::Exhausted);CHECK(!done.changed&&f.doc->generation()==generation&&f.current==prior);++worker_failures;
  }
  CHECK(worker_failures>1&&worker_failures<64);
  std::printf("checkpoint task allocation failures admission=%zu worker=%zu commit=%zu\n",admission_failures,worker_failures,commit_failures);
}
} // namespace
int main() {
  phase="selected";selected_only_and_history();phase="typed-project";typed_project_adoption();phase="profile";checkpoint_profile_binding();phase="material";material_binding_admission();phase="noop";identical_noop();phase="project-pin";displayed_project_pins();phase="closure";closure_failures();phase="stale";stale_and_cancellation();phase="owner";owner_and_admission();
  phase="destructor";destructor_retains_blocked_owners();phase="allocation";allocation_failures();
  std::puts("async selected mesh checkpoint admission, history, cancellation and ownership passed");
}
