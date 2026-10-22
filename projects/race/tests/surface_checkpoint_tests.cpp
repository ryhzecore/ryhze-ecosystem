#include "surface_checkpoint_support.hpp"
#include "../apps/desktop_profile.hpp"

using namespace surface_test;
int main(){
  for(bool scalar_material:{false,true})for(bool scalar:{false,true})for(bool mips:{false,true})for(bool textured:{false,true}){
    if(scalar_material&&(!scalar||!mips||!textured))continue;
    if(scalar&&(!mips||(textured&&!scalar_material)))continue;Data f(mips,textured,scalar,scalar_material);
    for(std::size_t n=0;n<2;++n){
      const auto& c=f.captured[n];auto loaded=surface_checkpoint::load(*f.checkpoints,c.leaf,f.view->project,*f.products);CHECK(loaded);
      CHECK(loaded.value().definition()==c.definition&&c.definition.index_revision==digest(f.recipes->data["project.raceproject"]));
      CHECK(std::equal(loaded.value().product().begin(),loaded.value().product().end(),f.cooked[n].begin(),f.cooked[n].end()));
      CHECK(loaded.value().texture().empty()==(n==0||!textured));
      if(scalar_material&&n==1){
        const auto material=sf::validate_material(loaded.value().product());CHECK(material);
        const auto texture=sf::validate_texture(loaded.value().texture());CHECK(texture);
        CHECK(material.value().schema()==3&&material.value().material().texture_role==sf::TextureRole::DiffuseReflectanceR);
        CHECK(texture.value().schema()==3&&texture.value().encoding()==sf::Encoding::LinearData);
        CHECK(sf::validate_binding(material.value(),texture.value()));
      }
      auto again=surface_checkpoint::capture(f.view->project,f.cooked[n],*f.products,*f.checkpoints);CHECK(again&&again.value().leaf==c.leaf);
      auto wrong=c.definition;wrong.recipe_revision[0]^=std::byte{1};auto raw=surface_checkpoint::encode(wrong);CHECK(raw);
      const auto leaf=hex(digest(bytes(raw.value())))+".json";put(*f.checkpoints,leaf,raw.value());CHECK(!surface_checkpoint::load(*f.checkpoints,leaf,f.view->project,*f.products));
      f.products->oversized=true;CHECK_ERROR(surface_checkpoint::load(*f.checkpoints,c.leaf,f.view->project,*f.products),ErrorCode::Exhausted);f.products->oversized=false;
      f.checkpoints->oversized=true;CHECK_ERROR(surface_checkpoint::load(*f.checkpoints,c.leaf,f.view->project,*f.products),ErrorCode::Exhausted);f.checkpoints->oversized=false;
      auto original=f.products->data[hex(digest(f.cooked[n]))+".rasset"];f.products->data[hex(digest(f.cooked[n]))+".rasset"].back()^=std::byte{1};
      CHECK(!surface_checkpoint::load(*f.checkpoints,c.leaf,f.view->project,*f.products));f.products->data[hex(digest(f.cooked[n]))+".rasset"]=std::move(original);
    }
    const auto leaf=hex(digest(f.cooked[0]))+".rasset";f.products->data.erase(leaf);
    auto result=surface_checkpoint::load(*f.checkpoints,f.captured[1].leaf,f.view->project,*f.products);CHECK(bool(result)==!textured);
    f.repin_index();auto other=f.clone();
    CHECK_ERROR(surface_checkpoint::load(*f.checkpoints,f.captured[1].leaf,other->project,*f.products),ErrorCode::InvalidDependency);
  }
  {
    Data scalar(true,true,true,true);const auto view=sf::validate_material(scalar.cooked[1]);CHECK(view);
    const auto retained=scalar.checkpoints->data;const auto writes=scalar.checkpoints->writes;
    for(unsigned mode=0;mode<4;++mode){
      auto stamp=view.value().stamp();auto material=view.value().material();
      if(mode==0){stamp=surface_pipeline::profile_info(surface_recipe::Profile::MaterialScalarMips).value().imported;
        stamp.source=view.value().stamp().source;stamp.dependencies=sf::dependency_digest(material.base_color_texture);}
      if(mode==1)material.texture_role=sf::TextureRole::Color;
      if(mode==2){material.texture_role=sf::TextureRole::Color;material.sampling=sf::Sampling::LinearClampMip0;}
      if(mode==3)stamp.settings[0]^=std::byte{1};
      auto invalid=sf::encode_material(view.value().id(),stamp,material);CHECK(invalid&&sf::validate_material(invalid.value()));
      put(*scalar.products,hex(digest(invalid.value()))+".rasset",invalid.value());
      CHECK_ERROR(surface_checkpoint::capture(scalar.view->project,invalid.value(),*scalar.products,*scalar.checkpoints),ErrorCode::InvalidDependency);
      CHECK(scalar.checkpoints->writes==writes&&scalar.checkpoints->data==retained);
    }
  }
  Data f;const auto raw=f.checkpoints->data[f.captured[1].leaf];
  for(std::size_t n=0;n+2<raw.size();++n)CHECK(!surface_checkpoint::decode(std::span(raw).first(n)));
  for(std::size_t n=0;n<256;++n){d::profile::testing::fail_allocation_after(n);auto loaded=surface_checkpoint::load(*f.checkpoints,f.captured[1].leaf,f.view->project,*f.products);d::profile::testing::fail_next_allocation_attempts(0);if(loaded)break;CHECK(loaded.error().code==ErrorCode::Exhausted);CHECK(n<255);}
  std::puts("surface checkpoint raw pins, full closure, corruption, capacities and allocation recovery passed");
}
