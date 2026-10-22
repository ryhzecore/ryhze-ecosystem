#include "desktop_surface_reimport.hpp"
#include "surface_checkpoint_support.hpp"
using namespace surface_test;
namespace {
d::SurfaceReimportCompletion finish(d::SurfaceReimportTask& task,Data& f,std::shared_ptr<const d::SurfaceReimportCheckpoint>& captured,assets::AssetId asset=aid(1),std::uint64_t epoch=1){
  for(unsigned i=0;i<10000;++i){auto p=task.poll(f.view,asset,epoch,captured);CHECK(p);if(p.value())return std::move(*p.value());std::this_thread::sleep_for(1ms);}throw std::runtime_error("surface import deadline");
}
}
int main(){
  for(bool scalar:{false,true})for(auto asset:{aid(1),aid(2)}){
    Data f(true,true,scalar,scalar);d::SurfaceReimportTask task(f.factory());std::shared_ptr<const d::SurfaceReimportCheckpoint> captured;
    d::SurfaceReimportRequest request{"sources","products","checkpoints",1};
    CHECK(task.start(f.view,asset,request));CHECK_ERROR(task.start(f.view,asset,request),ErrorCode::Busy);
    auto done=finish(task,f,captured,asset);CHECK(done.result&&done.changed&&captured&&done.retained_bytes<=d::maximum_surface_checkpoint_retained_bytes);
    CHECK(captured->published.leaf==f.captured[asset==aid(1)?0:1].leaf);
    if(scalar){
      auto loaded=surface_checkpoint::load(*f.checkpoints,captured->published.leaf,f.view->project,*f.products);CHECK(loaded);
      if(asset==aid(1)){
        const auto texture=sf::validate_texture(loaded.value().product());CHECK(texture&&texture.value().schema()==3&&texture.value().encoding()==sf::Encoding::LinearData);
      }else{
        const auto material=sf::validate_material(loaded.value().product());const auto texture=sf::validate_texture(loaded.value().texture());
        CHECK(material&&texture&&material.value().schema()==3&&material.value().material().texture_role==sf::TextureRole::DiffuseReflectanceR);
        CHECK(texture.value().schema()==3&&sf::validate_binding(material.value(),texture.value()));
      }
    }
    const auto retained=captured;
    if(scalar&&asset==aid(2)){
      const auto source=f.sources->data["material.json"];const auto products=f.products->data,checkpoints=f.checkpoints->data;
      std::string wrong(reinterpret_cast<const char*>(source.data()),source.size());
      const auto role=wrong.find("diffuse-reflectance-r");CHECK(role!=std::string::npos);wrong.replace(role,std::string_view("diffuse-reflectance-r").size(),"color");put(*f.sources,"material.json",wrong);
      CHECK(task.start(f.view,asset,request));done=finish(task,f,captured,asset);
      CHECK(!done.result&&!done.changed&&captured==retained&&f.products->data==products&&f.checkpoints->data==checkpoints);
      f.sources->data["material.json"]=source;
    }
    for(int mode=0;mode<5;++mode){
      f.sources->gate=std::make_shared<Gate>();f.sources->gated=asset==aid(1)?"color.ppm":"material.json";
      CHECK(task.start(f.view,asset,request));f.sources->gate->wait();CHECK(task.busy());CHECK(!task.poll(f.view,asset,1,captured).value());
      if(mode==0)CHECK(task.cancel());if(mode==1)f.view=f.clone();
      if(mode==4)f.products->fail_write=f.products->writes;
      f.sources->gate->release();done=finish(task,f,captured,mode==2?aid(9):asset,mode==3?2:1);
      CHECK(!done.result&&!done.changed&&captured==retained);f.sources->gate.reset();f.products->fail_write=SIZE_MAX;
    }
    f.sources->oversized=true;CHECK(task.start(f.view,asset,request));CHECK_ERROR(finish(task,f,captured,asset).result,ErrorCode::Exhausted);f.sources->oversized=false;
    f.recipes->data["texture.json"].back()^=std::byte{1};CHECK(task.start(f.view,asset,request));CHECK(!finish(task,f,captured,asset).result);f.recipes->data["texture.json"].back()^=std::byte{1};
    CHECK(task.start(f.view,asset,request));CHECK(finish(task,f,captured,asset).result);
    bool wrong=false;std::thread foreign([&]{wrong=!task.start(f.view,asset,request);});foreign.join();CHECK(wrong);
  }
  {
    Data f;auto task=std::make_unique<d::SurfaceReimportTask>(f.factory());
    f.sources->gate=std::make_shared<Gate>();f.sources->gated="color.ppm";const auto gate=f.sources->gate;
    CHECK(task->start(f.view,aid(1),{"sources","products","checkpoints",1}));gate->wait();
    std::weak_ptr<Files> source=f.sources;std::weak_ptr<const d::ProjectBrowserView> view=f.view;f.sources.reset();f.view.reset();
    std::atomic<bool> destroyed{};std::thread release([&]{std::this_thread::sleep_for(5ms);CHECK(!destroyed.load()&&!source.expired()&&!view.expired());gate->release();});
    task.reset();destroyed=true;release.join();CHECK(source.expired()&&view.expired());
  }
  std::puts("surface import full closure, immutable capture, joined cancellation and stale publication passed");
}
