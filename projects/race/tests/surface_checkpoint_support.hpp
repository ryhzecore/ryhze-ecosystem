#pragma once
#include "desktop_project_browser.hpp"
#include "surface_checkpoint.hpp"
#include "assets_support.hpp"
#include <array>

namespace surface_test {
namespace d=race::desktop;
namespace sf=race::assets::surface;
inline auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
inline void put(FakeFiles& files,const std::string& leaf,std::span<const std::byte> data){files.data[leaf]={data.begin(),data.end()};}
inline void put(FakeFiles& files,const std::string& leaf,std::string_view text){files.data[leaf]={bytes(text).begin(),bytes(text).end()};}
struct Files:FakeFiles {
  bool oversized{};
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override {
    auto value=FakeFiles::read(leaf,limit);if(value&&oversized)value.value().reserve(limit+1);return value;
  }
};
struct Data {
  std::shared_ptr<Files> recipes=std::make_shared<Files>(),sources=std::make_shared<Files>(),products=std::make_shared<Files>(),checkpoints=std::make_shared<Files>();
  std::shared_ptr<const d::ProjectBrowserView> view;
  std::array<std::vector<std::byte>,2> cooked;
  std::array<surface_checkpoint::Published,2> captured;
  explicit Data(bool mips=true,bool textured=true,bool scalar=false,bool scalar_material=false){
    CHECK(!scalar_material||(scalar&&mips&&textured));
    CHECK(!scalar||(mips&&(!textured||scalar_material)));
    using P=surface_recipe::Profile;
    std::string ppm="P6\n2 1\n255\n";ppm.append("\x12\x34\x56\x78\x9a\xbc",6);put(*sources,"color.ppm",ppm);
    auto texture=surface_recipe::encode({aid(1),"color.ppm",scalar?P::PpmScalarMips:mips?P::PpmSrgbMips:P::PpmSrgb});CHECK(texture);
    put(*recipes,"texture.json",texture.value()+" \n");auto snapshot=surface_recipe::load(*recipes,"texture.json");CHECK(snapshot);
    auto built=surface_recipe::build(snapshot.value(),sources->data["color.ppm"]);CHECK(built);cooked[0]=built.value().cooked;CHECK(surface_pipeline::publish(*products,cooked[0]));
    const sf::Reference pin{aid(1),digest(cooked[0])};
    const auto source=std::string("{\"schema\":")+(scalar_material?"3":mips&&textured?"2":"1")+",\"model\":\"opaque-diffuse\",\"base_color\":[1,0.5,1],\"base_color_texture\":"+
      (textured?"{\"asset\":\""+id_text(pin.asset)+"\",\"revision\":\""+hex(pin.revision)+"\"}":"null")+(mips&&textured?",\"sampling\":\"linear-clamp-mips\"":"")+
      (scalar_material?",\"texture_role\":\"diffuse-reflectance-r\"}":"}");
    put(*sources,"material.json",source);
    auto material=surface_recipe::encode({aid(2),"material.json",scalar_material?P::MaterialScalarMips:mips&&textured?P::MaterialMips:P::Material,textured?std::optional<sf::Reference>{pin}:std::nullopt});CHECK(material);
    put(*recipes,"material.json",material.value()+" \n");snapshot=surface_recipe::load(*recipes,"material.json");CHECK(snapshot);
    built=surface_recipe::build(snapshot.value(),sources->data["material.json"],products.get());CHECK(built);cooked[1]=built.value().cooked;CHECK(surface_pipeline::publish(*products,cooked[1]));
    std::array<content_project::Reference,2> refs{{{aid(1),content_project::Kind::Texture,"texture.json",digest(recipes->data["texture.json"])},{aid(2),content_project::Kind::Material,"material.json",digest(recipes->data["material.json"])}}};
    auto index=content_project::encode_binary(refs);CHECK(index);put(*recipes,"project.raceproject",index.value());view=clone();
    for(std::size_t n=0;n<2;++n){auto c=surface_checkpoint::capture(view->project,cooked[n],*products,*checkpoints);CHECK(c);captured[n]=std::move(c).value();}
  }
  void repin_index(){
    // Different canonical recipe leaf, exact same owned recipe bytes/pin.
    auto refs=std::vector<content_project::Reference>{};
    for(const auto& row:view->project.entries())refs.push_back(row.reference);
    recipes->data["alias.json"]=recipes->data.at(refs.back().recipe);refs.back().recipe="alias.json";
    auto index=content_project::encode_binary(refs);CHECK(index);put(*recipes,"project.raceproject",index.value());
  }
  std::shared_ptr<const d::ProjectBrowserView> clone(){
    auto loaded=content_project::load(*recipes,"project.raceproject");CHECK(loaded);return std::make_shared<d::ProjectBrowserView>(d::ProjectBrowserRequest{"recipes","project.raceproject",1},std::move(loaded).value());
  }
  d::ProjectBrowserTask::RootFactory factory() const {
    return [r=recipes,s=sources,p=products,c=checkpoints](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>>{
      if(name=="recipes")return std::shared_ptr<platform::RootedFiles>(r);if(name=="sources")return std::shared_ptr<platform::RootedFiles>(s);
      if(name=="products")return std::shared_ptr<platform::RootedFiles>(p);if(name=="checkpoints")return std::shared_ptr<platform::RootedFiles>(c);return fail(ErrorCode::NotFound,"Unknown fixture root");
    };
  }
};
}
