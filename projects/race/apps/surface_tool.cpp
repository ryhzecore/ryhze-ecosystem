#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "surface_pipeline.hpp"
#include "surface_recipe.hpp"
#include "authoring_json.hpp"
#include "race/build_identity.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

namespace {
using namespace race;
namespace s = assets::surface;
int error(Error e) { std::fprintf(stderr, "surface_error code=%u reason=%s\n", unsigned(e.code), e.message); return 1; }
int report(std::span<const std::byte> bytes, std::string_view leaf,
           const surface_recipe::Snapshot* recipe=nullptr, std::string_view imported_leaf={}) {
  auto texture = s::validate_texture(bytes);
  assets::AssetId id; s::Stamp stamp; std::uint32_t width{}, height{}, encoding{}, schema{}, mips{};
  std::size_t texel_bytes{}; const char* profile = "opaque-diffuse"; const char* sampling = "none";
  if (texture) {
    const auto& v=texture.value();id=v.id();stamp=v.stamp();width=v.width();height=v.height();encoding=static_cast<std::uint32_t>(v.encoding());
    schema=v.schema();mips=v.mip_count();texel_bytes=v.texels().size();profile=schema==3?"opaque-scalar-mips":schema==2?"opaque-srgb-mips":"rgba8-mip0";
  } else {
    auto material=s::validate_material(bytes);if(!material)return error(material.error());const auto& v=material.value();
    id=v.id();stamp=v.stamp();schema=v.schema();sampling=v.material().sampling==s::Sampling::LinearClampMips?"linear-clamp-mips":"linear-clamp-mip0";
  }
  std::string invocation;
  if(recipe) {
    auto info=surface_pipeline::profile_info(recipe->definition.profile);if(!info)return error(info.error());
    if(imported_leaf.size()!=71)return error(fail(ErrorCode::Invariant,"Invalid published imported surface name"));
    invocation=",\"recipe_revision\":\""+assets::hex(recipe->revision)+"\",\"recipe_profile\":\""+
      std::string(info.value().name)+"\",\"source\":\""+assets::hex(stamp.source)+
      "\",\"imported_revision\":\""+std::string(imported_leaf.substr(0,64))+
      "\",\"imported_leaf\":\""+std::string(imported_leaf)+"\"";
  }
  std::printf("{\"format\":\"RACEAST1\",\"kind\":\"%s\",\"schema\":%u,\"source_commit\":\"%s\",\"authored_sha256\":\"%s\","
              "\"id\":\"%s\",\"revision\":\"%s\",\"leaf\":\"%.*s\",\"representation\":%u,\"target\":%u,"
              "\"width\":%u,\"height\":%u,\"encoding\":%u,\"profile\":\"%s\",\"sampling\":\"%s\",\"mip_count\":%u,\"texel_bytes\":%zu%s}\n",
              texture?"texture":"material",schema,build::source_commit,build::authored_input_sha256,
              assets::id_text(id).c_str(),assets::hex(assets::digest(bytes)).c_str(),static_cast<int>(leaf.size()),leaf.data(),
              static_cast<unsigned>(stamp.representation),stamp.target,width,height,encoding,profile,sampling,mips,texel_bytes,invocation.c_str());
  return 0;
}
int recipe_command(int argc,char** argv) {
  const bool creating=std::string_view(argv[1])=="create-recipe";
  if((creating&&argc!=7&&argc!=9)||(!creating&&argc!=6))return 2;
  const std::string_view leaf(argv[3]);
  if(!platform::valid_content_leaf(leaf)||!leaf.ends_with(".json"))
    return error(fail(ErrorCode::InvalidInput,"Surface recipe requires a canonical JSON leaf"));
  auto recipes=platform::make_rooted_files(argv[2]);if(!recipes)return error(recipes.error());
  if(creating) {
    auto id=assets::parse_id(argv[4]);if(!id)return error(id.error());
    auto profile=surface_pipeline::parse_profile(argv[6]);if(!profile)return error(profile.error());
    surface_recipe::Definition definition{id.value(),argv[5],profile.value(),{}};
    if(argc==9) {
      auto texture_id=assets::parse_id(argv[7]);if(!texture_id)return error(texture_id.error());
      auto revision=authoring_json::hex(argv[8]);if(!revision)return error(revision.error());
      definition.texture=s::Reference{texture_id.value(),revision.value()};
    }
    auto encoded=surface_recipe::encode(definition);if(!encoded)return error(encoded.error());
    const auto bytes=std::as_bytes(std::span(encoded.value()));
    auto published=recipes.value()->publish_immutable(leaf,bytes);if(!published)return error(published.error());
    auto readback=surface_recipe::load(*recipes.value(),leaf);if(!readback)return error(readback.error());
    if(!std::equal(bytes.begin(),bytes.end(),readback.value().bytes.begin(),readback.value().bytes.end()))
      return error(fail(ErrorCode::IoFailure,"Surface recipe exact readback differs"));
    const auto info=surface_pipeline::profile_info(definition.profile);if(!info)return error(info.error());
    std::printf("{\"format\":\"RACESURFACE_RECIPE1\",\"schema\":1,\"id\":\"%s\",\"source\":\"%s\","
                "\"recipe_profile\":\"%.*s\",\"recipe_leaf\":\"%.*s\",\"recipe_revision\":\"%s\","
                "\"source_commit\":\"%s\",\"authored_sha256\":\"%s\"}\n",
                assets::id_text(definition.id).c_str(),definition.source.c_str(),static_cast<int>(info.value().name.size()),info.value().name.data(),
                static_cast<int>(leaf.size()),leaf.data(),assets::hex(readback.value().revision).c_str(),build::source_commit,build::authored_input_sha256);
    return 0;
  }
  // One immutable recipe snapshot and one bounded source read own this entire
  // invocation. No source/profile override or implicit newest dependency exists.
  auto recipe=surface_recipe::load(*recipes.value(),leaf);if(!recipe)return error(recipe.error());
  auto info=surface_pipeline::profile_info(recipe.value().definition.profile);if(!info)return error(info.error());
  auto sources=platform::make_rooted_files(argv[4]);if(!sources)return error(sources.error());
  auto output=platform::make_rooted_files(argv[5]);if(!output)return error(output.error());
  const auto limit=info.value().material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes;
  auto source=sources.value()->read(recipe.value().definition.source,limit);if(!source)return error(source.error());
  if(source.value().size()>limit||source.value().capacity()>limit)
    return error(fail(ErrorCode::Exhausted,"Surface recipe source capacity limit"));
  // Admission and dependency resolution finish before any output is published.
  // A later publication/readback failure may retain a complete immutable file.
  auto built=surface_recipe::build(recipe.value(),source.value(),output.value().get());if(!built)return error(built.error());
  auto imported=surface_pipeline::publish(*output.value(),built.value().imported);if(!imported)return error(imported.error());
  auto cooked=surface_pipeline::publish(*output.value(),built.value().cooked);if(!cooked)return error(cooked.error());
  return report(built.value().cooked,cooked.value(),&recipe.value(),imported.value());
}
int run(int argc,char** argv) {
  if(argc==2&&std::string_view(argv[1])=="--identity") { std::printf("%s %s\n",build::source_commit,build::authored_input_sha256);return 0; }
  if(argc<4) {
    std::fprintf(stderr,"usage: race_surface_tool texture SOURCE_ROOT source.ppm|source.bmp OUTPUT_ROOT ID srgb|linear\n"
                        "       race_surface_tool material SOURCE_ROOT source.json OUTPUT_ROOT ID\n"
                        "       race_surface_tool cook INPUT_ROOT imported.rasset OUTPUT_ROOT\n"
                        "       race_surface_tool cook-mips INPUT_ROOT imported.rasset OUTPUT_ROOT\n"
                        "       race_surface_tool cook-scalar-mips INPUT_ROOT imported.rasset OUTPUT_ROOT\n"
                        "       race_surface_tool inspect|validate ROOT product.rasset\n"
                        "       race_surface_tool create-recipe ROOT recipe.json ID source PROFILE [TEXTURE_ID TEXTURE_REVISION]\n"
                        "       race_surface_tool recipe RECIPE_ROOT recipe.json SOURCE_ROOT OUTPUT_ROOT\n"
                        "Material cooking resolves pinned textures from OUTPUT_ROOT.\n");return 2;
  }
  try {
    const std::string_view mode(argv[1]),leaf(argv[3]);
    if(mode=="create-recipe"||mode=="recipe")return recipe_command(argc,argv);
    const bool texture=mode=="texture",material=mode=="material",mips=mode=="cook-mips",scalar_mips=mode=="cook-scalar-mips",cooking=mode=="cook"||mips||scalar_mips;
    if((texture&&argc!=7)||(material&&argc!=6)||(cooking&&argc!=5)||
       (!texture&&!material&&!cooking&&(argc!=4||(mode!="inspect"&&mode!="validate"))))return 2;
    if(!platform::valid_content_leaf(leaf)||!(texture?(leaf.ends_with(".ppm")||leaf.ends_with(".bmp")):leaf.ends_with(material?".json":".rasset")))
      return error(fail(ErrorCode::InvalidInput,"Surface tool requires a canonical source or native leaf"));
    auto input=platform::make_rooted_files(argv[2]);if(!input)return error(input.error());
    auto bytes=input.value()->read(leaf,material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes);
    if(!bytes)return error(bytes.error());
    if(bytes.value().capacity()>(material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes))return error(fail(ErrorCode::Exhausted,"Surface source buffer capacity limit"));
    if(!texture&&!material&&!cooking)return report(bytes.value(),leaf);
    auto output=platform::make_rooted_files(argv[4]);if(!output)return error(output.error());
    Result<std::vector<std::byte>> product=fail(ErrorCode::Invariant,"Uninitialized surface tool product");
    if(cooking) product=scalar_mips?surface_pipeline::cook_scalar_mips(bytes.value()):mips?surface_pipeline::cook_mips(bytes.value()):surface_pipeline::cook(bytes.value(),output.value().get());
    else {
      auto id=assets::parse_id(argv[5]);if(!id)return error(id.error());
      if(material)product=surface_pipeline::import_material(id.value(),bytes.value());
      else {
        const std::string_view encoding(argv[6]);
        if(encoding!="srgb"&&encoding!="linear")return error(fail(ErrorCode::InvalidInput,"Texture encoding must be srgb or linear"));
        product=surface_pipeline::import_texture(id.value(),bytes.value(),encoding=="srgb"?s::Encoding::SrgbColor:s::Encoding::LinearData);
      }
    }
    if(!product)return error(product.error());
    auto published=surface_pipeline::publish(*output.value(),product.value());if(!published)return error(published.error());
    return report(product.value(),published.value());
  } catch(const std::bad_alloc&) { return error(fail(ErrorCode::Exhausted,"Surface tool allocation")); }
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
  if(argc<2||argc>9)return 2;
  try {
    std::array<std::string,9> arguments;std::array<char*,9> pointers{};
    for(int n=0;n<argc;++n) {
      const std::wstring_view value(argv[n]);if(value.empty()||value.size()>16384)return error(race::fail(race::ErrorCode::InvalidInput,"Surface argument length"));
      const int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
      if(length<=0)return error(race::fail(race::ErrorCode::InvalidInput,"Surface argument encoding"));
      auto& text=arguments[static_cast<std::size_t>(n)];text.resize(static_cast<std::size_t>(length));
      if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),length,nullptr,nullptr)!=length)
        return error(race::fail(race::ErrorCode::PlatformFailure,"Surface argument conversion"));
      pointers[static_cast<std::size_t>(n)]=text.data();
    }
    return run(argc,pointers.data());
  } catch(const std::bad_alloc&) {return error(race::fail(race::ErrorCode::Exhausted,"Surface arguments allocation"));}
}
#else
int main(int argc,char** argv){return run(argc,argv);}
#endif
