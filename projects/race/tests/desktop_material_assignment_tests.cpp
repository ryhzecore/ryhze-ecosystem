#include "desktop_material_assignment.hpp"
#include "surface_pipeline.hpp"
#include "assets_support.hpp"
#include <cstdio>

namespace {
namespace d=race::desktop;
auto material(FakeFiles& files){
  const std::string source="{\"schema\":1,\"model\":\"opaque-diffuse\",\"base_color\":[1,.5,0],\"base_color_texture\":null}";
  // Strict JSON uses a leading zero for fractional values.
  auto text=source;text.replace(text.find(".5"),2,"0.5");
  auto imported=surface_pipeline::import_material({11,12},std::as_bytes(std::span(text)));CHECK(imported);
  auto cooked=surface_pipeline::cook(imported.value());CHECK(cooked);
  auto leaf=surface_pipeline::publish(files,cooked.value());CHECK(leaf);return leaf.value();
}
d::MaterialAssignmentCompletion finish(d::MaterialAssignmentTask& task,d::Document& doc,PersistentId selected,bool allow=true){
  const auto deadline=std::chrono::steady_clock::now()+10s;
  while(std::chrono::steady_clock::now()<deadline){auto result=task.poll(doc,selected,allow);CHECK(result);if(result.value())return std::move(*result.value());std::this_thread::sleep_for(1ms);}
  throw std::runtime_error("Material worker completion deadline");
}
void run(){
  auto files=std::make_shared<FakeFiles>();const auto leaf=material(*files);
  auto doc=d::Document::create();CHECK(doc);auto row=doc.value()->objects().value().front();
  auto cache=d::MeshSet::prepare(files,{});CHECK(cache);
  d::MaterialAssignmentTask task;std::string oversized_leaf=leaf;oversized_leaf.reserve(1024);CHECK_ERROR(task.start(*doc.value(),row,cache.value(),std::move(oversized_leaf)),ErrorCode::Exhausted);
  const auto start_generation=doc.value()->generation();
  CHECK(task.start(*doc.value(),row,cache.value(),leaf));CHECK_ERROR(task.start(*doc.value(),row,cache.value(),leaf),ErrorCode::Busy);
  auto applied=finish(task,*doc.value(),row.id);CHECK(applied.result&&applied.prepared&&doc.value()->generation()>start_generation);
  CHECK(doc.value()->objects().value().front().material==applied.reference);
  CHECK(doc.value()->undo());CHECK(!doc.value()->objects().value().front().material);CHECK(doc.value()->redo());
  cache=applied.prepared;row=doc.value()->objects().value().front();
  for(int mode=0;mode<4;++mode){
    files->gate=std::make_shared<Gate>();files->gated=leaf;
    const auto before=doc.value()->generation();CHECK(task.start(*doc.value(),row,cache.value(),leaf));files->gate->wait();
    CHECK(!task.poll(*doc.value(),row.id,true).value());
    if(mode==0)CHECK(task.cancel());
    if(mode==1){auto edited=row;edited.color.x=.2;CHECK(doc.value()->edit(edited,before));}
    files->gate->release();auto done=finish(task,*doc.value(),mode==2?PersistentId{}:row.id,mode!=3);CHECK(!done.result&&!done.prepared);
    CHECK(done.result.error().code==(mode==1?ErrorCode::StaleHandle:ErrorCode::Cancelled));
    CHECK(doc.value()->generation()==before+(mode==1?1:0));
    row=doc.value()->objects().value().front();files->gate.reset();files->gated.clear();
  }
  CHECK(task.start(*doc.value(),row,cache.value(),"missing.rasset"));auto missing=finish(task,*doc.value(),row.id);CHECK(!missing.result&&missing.result.error().code==ErrorCode::NotFound);
  CHECK(doc.value()->start_play());CHECK_ERROR(task.start(*doc.value(),row,cache.value(),leaf),ErrorCode::Busy);CHECK(doc.value()->stop_play());
  auto foreign=row;foreign.id={9,9};CHECK_ERROR(task.start(*doc.value(),foreign,cache.value(),leaf),ErrorCode::StaleHandle);
  bool wrong_thread=false;std::thread other([&]{wrong_thread=!task.start(*doc.value(),row,cache.value(),leaf);});other.join();CHECK(wrong_thread);
  CHECK(task.start(*doc.value(),row,cache.value(),leaf));CHECK(finish(task,*doc.value(),row.id).result);
}
void mip_assignment_preservation(){
  namespace sf=race::assets::surface;
  auto files=std::make_shared<FakeFiles>();const auto plain=material(*files);
  auto doc=d::Document::create();CHECK(doc);auto row=doc.value()->objects().value().front();
  auto cache=d::MeshSet::prepare(files,{});CHECK(cache);d::MaterialAssignmentTask task;
  CHECK(task.start(*doc.value(),row,cache.value(),plain));auto initial=finish(task,*doc.value(),row.id);CHECK(initial.result);
  cache=initial.prepared;row=doc.value()->objects().value().front();const auto plain_ref=row.material;
  sf::Stamp stamp;stamp.representation=sf::Representation::Cooked;stamp.target=sf::portable_cpu_target;
  const auto label=std::string_view("mip-assignment");stamp.source=stamp.importer=stamp.cooker=stamp.settings=assets::digest(std::as_bytes(std::span(label)));
  const auto store=[&](std::span<const std::byte> bytes){auto leaf=assets::hex(assets::digest(bytes))+".rasset";files->data[leaf]={bytes.begin(),bytes.end()};return leaf;};
  const auto make_material=[&](AssetId id,sf::Reference texture,sf::Sampling sampling){
    sf::Material mat;mat.base_color_texture=texture;mat.sampling=sampling;auto provenance=stamp;provenance.dependencies=sf::dependency_digest(mat.base_color_texture);
    auto bytes=sf::encode_material(id,provenance,mat);CHECK(bytes);return std::move(bytes).value();
  };
  std::array<std::byte,4> pixel{std::byte{31},std::byte{63},std::byte{127},std::byte{255}};
  auto old_texture=sf::encode_texture({130,1},stamp,1,1,sf::Encoding::SrgbColor,pixel);CHECK(old_texture);store(old_texture.value());
  const sf::Reference old_pin{{130,1},assets::digest(old_texture.value())};
  auto legacy=make_material({131,1},old_pin,sf::Sampling::LinearClampMip0);auto legacy_leaf=store(legacy);
  CHECK(task.start(*doc.value(),row,cache.value(),legacy_leaf));auto legacy_done=finish(task,*doc.value(),row.id);CHECK(legacy_done.result);
  cache=legacy_done.prepared;row=doc.value()->objects().value().front();const auto prior_ref=row.material;
  const auto prior_packet=cache.value()->find_material(*prior_ref).value()->material;const auto generation=doc.value()->generation();
  auto invalid=make_material({131,2},old_pin,sf::Sampling::LinearClampMips);const auto invalid_leaf=store(invalid);
  CHECK(task.start(*doc.value(),row,cache.value(),invalid_leaf));auto rejected=finish(task,*doc.value(),row.id);
  CHECK(!rejected.result&&!rejected.prepared&&doc.value()->generation()==generation);
  CHECK(doc.value()->objects().value().front().material==prior_ref&&cache.value()->find_material(*prior_ref).value()->material==prior_packet);
  std::array<std::byte,20> chain;chain.fill(std::byte{255});chain[16]=std::byte{91};
  auto mip_texture=sf::encode_texture_mips({130,2},stamp,2,2,sf::Encoding::SrgbColor,chain);CHECK(mip_texture);const auto mip_leaf=store(mip_texture.value());
  const sf::Reference mip_pin{{130,2},assets::digest(mip_texture.value())};
  auto filtered=make_material({131,3},mip_pin,sf::Sampling::LinearClampMips);const auto filtered_leaf=store(filtered);
  files->gate=std::make_shared<Gate>();files->gated=filtered_leaf;
  CHECK(task.start(*doc.value(),row,cache.value(),filtered_leaf));files->gate->wait();files->gate->release();
  auto selection_changed=finish(task,*doc.value(),PersistentId{});CHECK_ERROR(selection_changed.result,ErrorCode::Cancelled);
  CHECK(!selection_changed.prepared&&doc.value()->generation()==generation&&doc.value()->objects().value().front().material==prior_ref);
  files->gate.reset();files->gated.clear();
  const auto saved_texture=files->data[mip_leaf];files->data.erase(mip_leaf);
  CHECK(task.start(*doc.value(),row,cache.value(),filtered_leaf));auto missing=finish(task,*doc.value(),row.id);CHECK_ERROR(missing.result,ErrorCode::NotFound);
  CHECK(!missing.prepared&&doc.value()->generation()==generation&&cache.value()->find_material(*prior_ref).value()->material==prior_packet);
  files->data[mip_leaf]=saved_texture;
  CHECK(task.start(*doc.value(),row,cache.value(),filtered_leaf));auto applied=finish(task,*doc.value(),row.id);CHECK(applied.result&&applied.prepared);
  CHECK(doc.value()->generation()>generation&&doc.value()->objects().value().front().material==applied.reference);
  auto packet=applied.prepared->find_material(applied.reference).value()->material;
  CHECK(packet->sampling()==render::TextureSampling::LinearClampMips&&packet->texture()->complete_mip_chain()&&packet->texture()->texels().size()==20);
  CHECK(doc.value()->undo());CHECK(doc.value()->objects().value().front().material==prior_ref);
  CHECK(doc.value()->undo());CHECK(doc.value()->objects().value().front().material==plain_ref);
  CHECK(doc.value()->redo());CHECK(doc.value()->redo());CHECK(doc.value()->objects().value().front().material==applied.reference);
  auto frame=d::scene_frame(*doc.value(),applied.prepared.get(),{});CHECK(frame);
  files->data.clear();cache.value().reset();initial.prepared.reset();legacy_done.prepared.reset();applied.prepared.reset();packet.reset();files.reset();
  CHECK(frame.value().draws[0].material->texture()->texels()[16]==std::byte{91});
}

}
int main(){try{run();mip_assignment_preservation();std::puts("material assignment: joined worker, transaction/history, cancellation, stale selection and failure recovery passed");return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
