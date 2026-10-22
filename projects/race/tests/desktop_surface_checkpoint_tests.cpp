#include "desktop_surface_checkpoint.hpp"
#include "surface_checkpoint_support.hpp"
using namespace surface_test;
namespace {
d::SurfaceCheckpointCompletion finish(d::SurfaceCheckpointTask& task,Data& f,d::Document& doc,std::shared_ptr<const d::MeshSet>& cache,PersistentId selection,assets::AssetId asset=aid(2),std::uint64_t epoch=1,bool allow=true){
  for(unsigned i=0;i<10000;++i){auto p=task.poll(doc,cache,selection,f.view,asset,epoch,allow);CHECK(p);if(p.value())return std::move(*p.value());std::this_thread::sleep_for(1ms);}throw std::runtime_error("surface adoption deadline");
}
}
int main(){
  for(const bool scalar:{false,true}){
  Data f(true,true,scalar,scalar);auto made=d::Document::create();CHECK(made);auto& doc=*made.value();const auto selected=doc.objects().value().front().id;
  auto prepared=d::MeshSet::prepare(f.products,{});CHECK(prepared);auto cache=prepared.value();d::SurfaceCheckpointTask task(f.factory());
  d::SurfaceCheckpointRequest request{"checkpoints",f.captured[1].leaf,aid(2),1};
  CHECK(task.start(doc,cache,selected,f.view,request));CHECK_ERROR(task.start(doc,cache,selected,f.view,request),ErrorCode::Busy);
  auto done=finish(task,f,doc,cache,selected);CHECK(done.result&&done.changed&&cache->materials().size()==1&&cache->textures().size()==1);
  const auto packet=cache->materials()[0].material;
  CHECK(packet->texture_role()==(scalar?render::TextureRole::DiffuseReflectanceR:render::TextureRole::Color));
  CHECK(packet->texture()->encoding()==(scalar?render::TextureEncoding::LinearData:render::TextureEncoding::SrgbColor));
  const auto material=doc.objects().value().front().material;CHECK(material&&material->revision==digest(f.cooked[1]));CHECK(doc.undo());CHECK(!doc.objects().value().front().material);CHECK(doc.redo());
  const auto generation=doc.generation();CHECK(task.start(doc,cache,selected,f.view,request));done=finish(task,f,doc,cache,selected);CHECK(done.result&&!done.changed&&doc.generation()==generation);
  if(scalar){
    const auto view=sf::validate_material(f.cooked[1]);CHECK(view);const auto before=cache;
    for(unsigned mode=0;mode<3;++mode){
      auto stamp=view.value().stamp();auto value=view.value().material();
      if(mode==0){stamp=surface_pipeline::profile_info(surface_recipe::Profile::MaterialScalarMips).value().imported;
        stamp.source=view.value().stamp().source;stamp.dependencies=sf::dependency_digest(value.base_color_texture);}
      if(mode==1)value.texture_role=sf::TextureRole::Color;
      if(mode==2)stamp.settings[0]^=std::byte{1};
      auto product=sf::encode_material(aid(2),stamp,value);CHECK(product);
      auto definition=f.captured[1].definition;definition.cooked_revision=digest(product.value());
      const auto manifest=surface_checkpoint::encode(definition);CHECK(manifest);
      const auto leaf=hex(digest(bytes(manifest.value())))+".json";
      put(*f.products,hex(definition.cooked_revision)+".rasset",product.value());put(*f.checkpoints,leaf,manifest.value());
      auto invalid=request;invalid.checkpoint_leaf=leaf;CHECK(task.start(doc,cache,selected,f.view,invalid));
      done=finish(task,f,doc,cache,selected);
      CHECK(!done.result&&!done.changed&&cache==before&&doc.generation()==generation&&doc.objects().value().front().material==material);
    }
  }
  for(int mode=0;mode<7;++mode){
    f.checkpoints->gate=std::make_shared<Gate>();f.checkpoints->gated=request.checkpoint_leaf;const auto before=cache;
    CHECK(task.start(doc,cache,selected,f.view,request));f.checkpoints->gate->wait();CHECK(!task.poll(doc,cache,selected,f.view,aid(2),1).value());
    if(mode==0)CHECK(task.cancel());if(mode==1)f.view=f.clone();if(mode==5){CHECK(doc.undo());CHECK(doc.redo());}
    f.checkpoints->gate->release();done=finish(task,f,doc,cache,mode==2?PersistentId{}:selected,mode==3?aid(1):aid(2),mode==4?2:1,mode!=6);
    CHECK(!done.result&&!done.changed&&cache==before&&doc.objects().value().front().material==material);f.checkpoints->gate.reset();
  }
  auto texture=request;texture.asset=aid(1);texture.checkpoint_leaf=f.captured[0].leaf;CHECK_ERROR(task.start(doc,cache,selected,f.view,texture),ErrorCode::InvalidInput);
  f.products->data.erase(hex(digest(f.cooked[0]))+".rasset");CHECK(task.start(doc,cache,selected,f.view,request));CHECK_ERROR(finish(task,f,doc,cache,selected).result,ErrorCode::NotFound);
  CHECK(doc.start_play());CHECK_ERROR(task.start(doc,cache,selected,f.view,request),ErrorCode::Busy);CHECK(doc.stop_play());
  }
  {
    Data data;auto task_owned=std::make_unique<d::SurfaceCheckpointTask>(data.factory());auto document=d::Document::create();CHECK(document);
    auto packets=d::MeshSet::prepare(data.products,{});CHECK(packets);auto view=data.view;
    data.checkpoints->gate=std::make_shared<Gate>();data.checkpoints->gated=data.captured[1].leaf;const auto gate=data.checkpoints->gate;
    CHECK(task_owned->start(*document.value(),packets.value(),document.value()->objects().value().front().id,view,{"checkpoints",data.captured[1].leaf,aid(2),1}));gate->wait();
    std::weak_ptr<const d::MeshSet> held=packets.value();std::weak_ptr<const d::ProjectBrowserView> project=view;packets.value().reset();view.reset();data.view.reset();
    std::atomic<bool> destroyed{};std::thread release([&]{std::this_thread::sleep_for(5ms);CHECK(!destroyed.load()&&!held.expired()&&!project.expired());gate->release();});
    task_owned.reset();destroyed=true;release.join();CHECK(held.expired()&&project.expired());
  }
  std::puts("material checkpoint Scene/cache/history atomicity, stale contexts, exact texture closure and cancellation passed");
}
