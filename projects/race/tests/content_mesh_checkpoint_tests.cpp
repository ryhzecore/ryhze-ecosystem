#include "content_mesh_checkpoint.hpp"
#include "mesh_reimport.hpp"
#include "surface_checkpoint_support.hpp"
using namespace surface_test;
int main(){
  for(auto profile:{mesh_pipeline::Profile::Geometry,mesh_pipeline::Profile::GeometryUv}){
    Data f;const std::string source=profile==mesh_pipeline::Profile::Geometry?"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n":"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
    auto recipe=mesh_recipe::encode({aid(3),"mesh.obj",profile});CHECK(recipe);put(*f.recipes,"mesh.json",recipe.value());
    std::array<mesh_project::Reference,1> mesh_refs{{{aid(3),"mesh.json",digest(bytes(recipe.value()))}}};
    auto mesh_index=mesh_project::encode(mesh_refs);CHECK(mesh_index);put(*f.recipes,"mesh-only.raceproject",mesh_index.value());
    auto mesh_only=mesh_project::load(*f.recipes,"mesh-only.raceproject");CHECK(mesh_only);auto adapted=content_project::load(*f.recipes,"mesh-only.raceproject");CHECK(adapted);
    auto imported=mesh_pipeline::import_obj(aid(3),bytes(source),profile);CHECK(imported);auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);CHECK(mesh_pipeline::publish(*f.products,cooked.value()));
    auto memory=TrackedAllocator::create({64*1024*1024});CHECK(memory);auto owner=mesh_import::Reimport::create(1,aid(3),f.products,memory.value(),{},profile);CHECK(owner);CHECK(owner.value()->restore_initial(cooked.value()));
    auto lease=owner.value()->acquire(owner.value()->current().value());CHECK(lease);
    auto old=mesh_checkpoint::capture(mesh_only.value(),lease.value(),*f.products,*f.checkpoints);CHECK(old);
    auto equivalent=mesh_checkpoint::capture(adapted.value(),lease.value(),*f.products,*f.checkpoints);CHECK(equivalent&&equivalent.value().leaf==old.value().leaf);
    std::vector<content_project::Reference> refs;for(const auto& row:f.view->project.entries())refs.push_back(row.reference);
    refs.push_back({aid(3),content_project::Kind::Mesh,"mesh.json",digest(bytes(recipe.value()))});
    auto index=content_project::encode_binary(refs);CHECK(index);put(*f.recipes,"project.raceproject",index.value());f.view=f.clone();
    auto captured=mesh_checkpoint::capture(f.view->project,lease.value(),*f.products,*f.checkpoints);CHECK(captured);
    CHECK(captured.value().definition.index_revision==digest(f.recipes->data["project.raceproject"]));
    auto loaded=mesh_checkpoint::load(*f.checkpoints,captured.value().leaf,f.view->project,*f.products);CHECK(loaded);
    CHECK(std::equal(loaded.value().product().begin(),loaded.value().product().end(),cooked.value().begin(),cooked.value().end()));
    CHECK(!mesh_checkpoint::load(*f.checkpoints,old.value().leaf,f.view->project,*f.products));
    f.repin_index();auto changed=f.clone();CHECK(!mesh_checkpoint::load(*f.checkpoints,captured.value().leaf,changed->project,*f.products));
    f.recipes->data["material.json"].back()^=std::byte{1};CHECK(!content_project::load(*f.recipes,"project.raceproject"));
  }
  std::puts("typed mesh checkpoints share binary project bytes, exact typed pins, full closure and both mesh profiles");
}
