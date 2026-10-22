#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "content_project.hpp"
#include "authoring_json.hpp"
#include "race/build_identity.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

namespace {
using namespace race;
namespace cp=content_project;
int error(Error e) {
  std::fprintf(stderr,"content_error code=%u reason=%s\n",unsigned(e.code),e.message);return 1;
}
Result<cp::Kind> kind(std::string_view name) {
  if(name=="mesh")return cp::Kind::Mesh;
  if(name=="texture")return cp::Kind::Texture;
  if(name=="material")return cp::Kind::Material;
  return fail(ErrorCode::InvalidInput,"Content kind must be mesh, texture or material");
}
const char* kind_name(cp::Kind value) {
  return value==cp::Kind::Mesh?"mesh":value==cp::Kind::Texture?"texture":"material";
}
int report(const cp::Snapshot& project,std::string_view leaf) {
  // Complete serialisation before stdout publication; allocation failure cannot
  // masquerade as a successful truncated project receipt.
  std::string rows="[";
  for(const auto& entry:project.entries()) {
    std::string_view source,profile;
    if(const auto* mesh=std::get_if<mesh_recipe::Snapshot>(&entry.recipe)) {
      source=mesh->definition.source;
      profile=mesh->definition.profile==mesh_pipeline::Profile::GeometryUv?"geometry-uv":"geometry";
    } else {
      const auto& surface=std::get<surface_recipe::Snapshot>(entry.recipe);
      source=surface.definition.source;
      auto info=surface_pipeline::profile_info(surface.definition.profile);if(!info)return error(info.error());
      profile=info.value().name;
    }
    if(rows.size()>1)rows+=',';
    rows+="{\"kind\":\""+std::string(kind_name(entry.reference.kind))+"\",\"id\":\""+
      assets::id_text(entry.reference.id)+"\",\"source\":\""+std::string(source)+
      "\",\"recipe\":\""+entry.reference.recipe+"\",\"recipe_revision\":\""+
      assets::hex(entry.reference.revision)+"\",\"profile\":\""+std::string(profile)+"\"}";
  }
  rows+=']';
  std::printf("{\"format\":\"RACECONTENT_PROJECT\",\"schema\":%u,\"encoding\":\"%s\",\"wire_version\":%u,\"source_commit\":\"%s\",\"authored_sha256\":\"%s\","
              "\"leaf\":\"%.*s\",\"revision\":\"%s\",\"entries\":%zu,\"retained_bytes\":%zu,\"recipes\":%s}\n",
              project.schema(),"binary",cp::binary_version,
              build::source_commit,build::authored_input_sha256,static_cast<int>(leaf.size()),leaf.data(),
              assets::hex(project.revision()).c_str(),project.entries().size(),project.retained_bytes(),rows.c_str());
  return 0;
}
int run(int argc,char** argv) {
  if(argc==2&&std::string_view(argv[1])=="--identity") {
    std::printf("%s %s\n",build::source_commit,build::authored_input_sha256);return 0;
  }
  if(argc<4) {
    std::fprintf(stderr,"usage: race_content_tool create ROOT index.raceproject [KIND ID recipe.json RECIPE_REVISION]...\n"
                        "       race_content_tool inspect ROOT index.raceproject\n"
                        "Kinds: mesh, texture, material. Publication validates the complete pinned recipe closure.\n");
    return 2;
  }
  try {
    const std::string_view mode(argv[1]),leaf(argv[3]);
    const bool creating=mode=="create";
    if((mode=="inspect"&&argc!=4)||(!creating&&mode!="inspect")||
       (creating&&((argc-4)%4!=0||static_cast<std::size_t>((argc-4)/4)>cp::maximum_entries)))return 2;
    if(!cp::valid_index_leaf(leaf))
      return error(fail(ErrorCode::InvalidInput,"Content project requires a canonical .raceproject leaf"));
    auto root=platform::make_rooted_files(argv[2]);if(!root)return error(root.error());
    if(mode=="inspect") {
      auto loaded=cp::load(*root.value(),leaf);if(!loaded)return error(loaded.error());
      return report(loaded.value(),leaf);
    }
    std::vector<cp::Reference> references;
    references.reserve(static_cast<std::size_t>((argc-4)/4));
    if(references.capacity()>cp::maximum_entries)return error(fail(ErrorCode::Exhausted,"Content project reference capacity"));
    for(int n=4;n<argc;n+=4) {
      auto selected=kind(argv[n]);if(!selected)return error(selected.error());
      auto id=assets::parse_id(argv[n+1]);if(!id)return error(id.error());
      auto revision=authoring_json::hex(argv[n+3]);if(!revision)return error(revision.error());
      references.push_back({id.value(),selected.value(),argv[n+2],revision.value()});
    }
    auto binary=cp::encode_binary(references);if(!binary)return error(binary.error());
    auto encoded=std::move(binary).value();
    if(encoded.capacity()>cp::maximum_bytes)return error(fail(ErrorCode::Exhausted,"Content project encoded capacity"));
    const std::span<const std::byte> bytes=encoded;
    auto admitted=cp::Snapshot::open(bytes,*root.value());if(!admitted)return error(admitted.error());
    auto published=root.value()->publish_immutable(leaf,bytes);if(!published)return error(published.error());
    auto readback=cp::load(*root.value(),leaf);if(!readback)return error(readback.error());
    if(!std::equal(bytes.begin(),bytes.end(),readback.value().bytes().begin(),readback.value().bytes().end()))
      return error(fail(ErrorCode::IoFailure,"Content project exact readback differs"));
    return report(readback.value(),leaf);
  }catch(const std::bad_alloc&) {return error(fail(ErrorCode::Exhausted,"Content tool allocation"));}
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
  constexpr std::size_t maximum_arguments=4+4*content_project::maximum_entries;
  if(argc<2||static_cast<std::size_t>(argc)>maximum_arguments)return 2;
  try {
    std::array<std::string,maximum_arguments> arguments;
    std::array<char*,maximum_arguments> pointers{};
    for(int n=0;n<argc;++n) {
      const std::wstring_view value(argv[n]);
      if(value.empty()||value.size()>16384)return error(fail(ErrorCode::InvalidInput,"Content argument length"));
      const int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
      if(length<=0)return error(fail(ErrorCode::InvalidInput,"Content argument encoding"));
      auto& text=arguments[static_cast<std::size_t>(n)];text.resize(static_cast<std::size_t>(length));
      if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),length,nullptr,nullptr)!=length)
        return error(fail(ErrorCode::PlatformFailure,"Content argument conversion"));
      pointers[static_cast<std::size_t>(n)]=text.data();
    }
    return run(argc,pointers.data());
  }catch(const std::bad_alloc&) {return error(fail(ErrorCode::Exhausted,"Content argument allocation"));}
}
#else
int main(int argc,char** argv){return run(argc,argv);}
#endif
