#include "desktop_package.hpp"
#include "race/build_identity.hpp"
#include <cstdio>

namespace {
int error(race::Error e){std::fprintf(stderr,"package_error code=%u reason=%s\n",unsigned(e.code),e.message);return 1;}
}
int main(int argc,char** argv){
  using namespace race;
  if(argc==2&&std::string_view(argv[1])=="--identity"){
    std::printf("%s %s\n",build::source_commit,build::authored_input_sha256);return 0;
  }
  if(argc!=4&&argc!=6){
    std::fprintf(stderr,"usage: race_package_tool pack INPUT_ROOT scene.rscene OUTPUT_ROOT SCENE_ID\n"
                        "       race_package_tool inspect|validate ROOT content.rpak\n");return 2;
  }
  try{
    const std::string_view mode(argv[1]),leaf(argv[3]);const bool packing=mode=="pack";
    if((packing&&argc!=6)||(!packing&&(argc!=4||(mode!="inspect"&&mode!="validate"))))return 2;
    if(!platform::valid_content_leaf(leaf)||!leaf.ends_with(packing?".rscene":".rpak"))return error(fail(ErrorCode::InvalidInput,"canonical native content leaf required"));
    auto files=platform::make_rooted_files(argv[2]);if(!files)return error(files.error());
    auto document=desktop::Document::create();if(!document)return error(document.error());
    Result<std::vector<std::byte>> bytes=fail(ErrorCode::Invariant,"no package input");std::string output_leaf(leaf);
    if(packing){
      auto id=assets::parse_id(argv[5]);if(!id)return error(id.error());
      bytes=desktop::pack_scene(*files.value(),leaf,id.value(),*document.value()->schemas());if(!bytes)return error(bytes.error());
      auto output=platform::make_rooted_files(argv[4]);if(!output)return error(output.error());
      auto published=desktop::publish_package(*output.value(),bytes.value(),*document.value()->schemas());if(!published)return error(published.error());output_leaf=std::move(published).value();
    }else bytes=files.value()->read(leaf,assets::pak::Limits{}.package_bytes);
    if(!bytes)return error(bytes.error());
    auto prepared=desktop::prepare_package(bytes.value(),*document.value()->schemas());if(!prepared)return error(prepared.error());
    std::printf("{\"format\":\"RACEPAK1\",\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\",\"leaf\":\"%s\",\"bytes\":%zu,\"sha256\":\"%s\",\"scene_id\":\"%s\",\"scene_revision\":\"%s\",\"meshes\":%zu,\"materials\":%zu,\"textures\":%zu}\n",
      build::source_commit,build::authored_input_sha256,output_leaf.c_str(),bytes.value().size(),assets::hex(assets::digest(bytes.value())).c_str(),
      assets::id_text(prepared.value().scene_key.id).c_str(),assets::hex(prepared.value().scene_key.revision).c_str(),prepared.value().meshes->entries().size(),prepared.value().meshes->materials().size(),prepared.value().meshes->textures().size());return 0;
  }catch(const std::bad_alloc&){return error(fail(ErrorCode::Exhausted,"package tool allocation"));}
}
