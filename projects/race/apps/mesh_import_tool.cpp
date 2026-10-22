#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mesh_import_job.hpp"
#include "mesh_watch_cli.hpp"
#include "mesh_recipe.hpp"
#include "mesh_project.hpp"
#include "mesh_checkpoint.hpp"
#include "mesh_reimport.hpp"
#include "race/build_identity.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace {
using namespace race;
Result<std::string> utf8(std::wstring_view value){
  if(value.empty()||value.size()>16384)return fail(ErrorCode::InvalidInput,"Worker tool argument length");
  const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  if(size<=0)return fail(ErrorCode::InvalidInput,"Worker tool argument encoding");
  std::string out(static_cast<std::size_t>(size),'\0');
  if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),size,nullptr,nullptr)!=size)
    return fail(ErrorCode::PlatformFailure,"Worker tool argument conversion");
  return out;
}
int error(Error reason){std::fprintf(stderr,"mesh_import_error code=%u reason=%s\n",unsigned(reason.code),reason.message);return 1;}
int create_recipe(int argc,wchar_t** argv){
  const auto profile=argc==6?mesh_pipeline::Profile::Geometry:mesh_pipeline::Profile::GeometryUv;
  if(argc==7&&std::wstring_view(argv[6])!=L"geometry-uv")return error(fail(ErrorCode::InvalidInput,"Unknown mesh recipe profile"));
  std::array<std::string,4> args;
  for(std::size_t n=0;n<args.size();++n){auto converted=utf8(argv[n+2]);if(!converted)return error(converted.error());args[n]=std::move(converted).value();}
  if(!platform::valid_content_leaf(args[1])||!std::string_view(args[1]).ends_with(".json"))return error(fail(ErrorCode::InvalidInput,"Recipe requires a canonical JSON leaf"));
  auto id=assets::parse_id(args[2]);if(!id)return error(id.error());
  auto encoded=mesh_recipe::encode({id.value(),args[3],profile});if(!encoded)return error(encoded.error());
  auto files=platform::make_rooted_files(args[0]);if(!files)return error(files.error());
  auto bytes=std::as_bytes(std::span(encoded.value()));auto written=files.value()->publish_immutable(args[1],bytes);if(!written)return error(written.error());
  auto readback=mesh_recipe::load(*files.value(),args[1]);if(!readback)return error(readback.error());
  if(!std::equal(bytes.begin(),bytes.end(),readback.value().bytes.begin(),readback.value().bytes.end()))return error(fail(ErrorCode::IoFailure,"Recipe readback mismatch"));
  std::printf("{\"authored_sha256\":\"%s\",\"recipe_revision\":\"%s\",\"asset\":\"%s\",\"source\":\"%s\",\"leaf\":\"%s\"}\n",
    build::authored_input_sha256,assets::hex(readback.value().revision).c_str(),args[2].c_str(),args[3].c_str(),args[1].c_str());return 0;
}
int create_project(int argc,wchar_t** argv){
  auto root=utf8(argv[2]),leaf=utf8(argv[3]);if(!root)return error(root.error());if(!leaf)return error(leaf.error());
  if(!mesh_project::valid_index_leaf(leaf.value()))return error(fail(ErrorCode::InvalidInput,"Project requires a canonical .raceproject leaf"));
  auto files=platform::make_rooted_files(root.value());if(!files)return error(files.error());
  std::vector<mesh_project::Reference> references;references.reserve(static_cast<std::size_t>(argc-4));
  for(int n=4;n<argc;++n){
    auto name=utf8(argv[n]);if(!name)return error(name.error());auto recipe=mesh_recipe::load(*files.value(),name.value());if(!recipe)return error(recipe.error());
    references.push_back({recipe.value().definition.id,std::move(name).value(),recipe.value().revision});
  }
  auto encoded=mesh_project::encode(references);if(!encoded)return error(encoded.error());
  const auto bytes=std::as_bytes(std::span(encoded.value()));auto published=files.value()->publish_immutable(leaf.value(),bytes);if(!published)return error(published.error());
  auto readback=mesh_project::load(*files.value(),leaf.value());if(!readback)return error(readback.error());
  if(!std::equal(bytes.begin(),bytes.end(),readback.value().bytes().begin(),readback.value().bytes().end()))return error(fail(ErrorCode::IoFailure,"Project readback mismatch"));
  std::printf("{\"authored_sha256\":\"%s\",\"index_revision\":\"%s\",\"entries\":%zu,\"retained_bytes\":%zu}\n",build::authored_input_sha256,assets::hex(readback.value().revision()).c_str(),readback.value().entries().size(),readback.value().retained_bytes());return 0;
}
int load_checkpoint(wchar_t** argv){
  std::array<std::string,5> args;
  for(std::size_t n=0;n<args.size();++n){auto text=utf8(argv[n+2]);if(!text)return error(text.error());args[n]=std::move(text).value();}
  auto recipes=platform::make_rooted_files(args[0]);if(!recipes)return error(recipes.error());
  auto project=mesh_project::load(*recipes.value(),args[1]);if(!project)return error(project.error());
  auto products=platform::make_rooted_files(args[2]),checkpoints=platform::make_rooted_files(args[3]);
  if(!products)return error(products.error());if(!checkpoints)return error(checkpoints.error());
  auto loaded=mesh_checkpoint::load(*checkpoints.value(),args[4],project.value(),*products.value());if(!loaded)return error(loaded.error());
  auto allocator=TrackedAllocator::create({64*1024*1024});if(!allocator)return error(allocator.error());
  auto selected=project.value().find(loaded.value().definition().id);if(!selected)return error(selected.error());
  auto owner=mesh_import::Reimport::create(1,loaded.value().definition().id,products.value(),allocator.value(),{},selected.value()->recipe.definition.profile);if(!owner)return error(owner.error());
  auto restored=owner.value()->restore_initial(loaded.value().product());if(!restored)return error(restored.error());
  auto lease=owner.value()->acquire(restored.value());if(!lease)return error(lease.error());
  const auto& view=lease.value()->view();const auto& definition=loaded.value().definition();
  if(!std::equal(view.bytes().begin(),view.bytes().end(),loaded.value().product().begin(),loaded.value().product().end()))return error(fail(ErrorCode::Invariant,"Checkpoint restored bytes differ"));
  const int written=std::printf("{\"authored_sha256\":\"%s\",\"checkpoint_revision\":\"%s\",\"index_revision\":\"%s\",\"recipe_revision\":\"%s\",\"asset\":\"%s\",\"cooked_revision\":\"%s\",\"source\":\"%s\",\"loaded_capacity_bytes\":%zu,\"store_bytes\":%zu}\n",
    build::authored_input_sha256,assets::hex(loaded.value().revision()).c_str(),assets::hex(definition.index_revision).c_str(),assets::hex(definition.recipe_revision).c_str(),
    assets::id_text(definition.id).c_str(),assets::hex(view.revision()).c_str(),assets::hex(view.stamp().source).c_str(),loaded.value().retained_bytes(),owner.value()->residency().retained_bytes);
  if(written<0||std::fflush(stdout)!=0)return error(fail(ErrorCode::IoFailure,"Checkpoint load receipt output failed"));return 0;
}
}
int wmain(int argc,wchar_t** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(argc==2&&std::wstring_view(argv[1])==L"--identity"){
    std::printf("%s %s\n",build::source_commit,build::authored_input_sha256);return 0;
  }
  if(argc>=2&&(std::wstring_view(argv[1])==L"--watch"||std::wstring_view(argv[1])==L"--watch-recipe"||std::wstring_view(argv[1])==L"--watch-project"))return mesh_import::watch_tool(argc,argv);
  const bool recipe_mode=argc>=2&&std::wstring_view(argv[1])==L"--recipe";
  const bool project_mode=argc>=2&&std::wstring_view(argv[1])==L"--project";
  const bool create_definition=argc>=2&&std::wstring_view(argv[1])==L"--create-recipe";
  const bool create_index=argc>=2&&std::wstring_view(argv[1])==L"--create-project";
  const bool load_saved=argc>=2&&std::wstring_view(argv[1])==L"--load-checkpoint";
  if((create_index&&(argc<4||argc>36))||(create_definition&&argc!=6&&argc!=7)||(!create_index&&!create_definition&&argc!=(load_saved?7:project_mode?8:recipe_mode?7:6))){
    std::fprintf(stderr,"usage: race_mesh_import_tool SOURCE_ROOT source.obj OUTPUT_ROOT FRESH_STAGE_ROOT ID\n"
      "       race_mesh_import_tool --create-recipe ROOT recipe.json ID source.obj [geometry-uv]\n"
      "       race_mesh_import_tool --recipe RECIPE_ROOT recipe.json SOURCE_ROOT OUTPUT_ROOT FRESH_STAGE_ROOT\n"
      "       race_mesh_import_tool --create-project RECIPE_ROOT project.raceproject [recipe.json ...]\n"
      "       race_mesh_import_tool --project RECIPE_ROOT project.raceproject ID SOURCE_ROOT OUTPUT_ROOT FRESH_STAGE_ROOT\n"
      "       race_mesh_import_tool --load-checkpoint RECIPE_ROOT project.raceproject PRODUCT_ROOT CHECKPOINT_ROOT CHECKPOINT_LEAF\n");return 2;
  }
  try{
    if(create_index)return create_project(argc,argv);
    if(load_saved)return load_checkpoint(argv);
    if(create_definition)return create_recipe(argc,argv);
    using Clock=std::chrono::steady_clock;
    const auto begin=Clock::now();
    std::array<std::string,5> args;
    std::optional<mesh_recipe::Snapshot> recipe;
    std::optional<mesh_project::Snapshot> project;
    const mesh_recipe::Snapshot* selected{};
    if(recipe_mode||project_mode){
      auto root=utf8(argv[2]),leaf=utf8(argv[3]);if(!root)return error(root.error());if(!leaf)return error(leaf.error());
      auto files=platform::make_rooted_files(root.value());if(!files)return error(files.error());
      if(project_mode){
        auto loaded=mesh_project::load(*files.value(),leaf.value());if(!loaded)return error(loaded.error());project=std::move(loaded).value();
        auto identity=utf8(argv[4]);if(!identity)return error(identity.error());auto id=assets::parse_id(identity.value());if(!id)return error(id.error());
        auto found=project->find(id.value());if(!found)return error(found.error());selected=&found.value()->recipe;
      }else{auto loaded=mesh_recipe::load(*files.value(),leaf.value());if(!loaded)return error(loaded.error());recipe=std::move(loaded).value();selected=&*recipe;}
      const unsigned shift=project_mode?1:0;
      for(auto pair:{std::pair{0U,4U},std::pair{2U,5U},std::pair{3U,6U}}){auto converted=utf8(argv[pair.second+shift]);if(!converted)return error(converted.error());args[pair.first]=std::move(converted).value();}
      args[1]=selected->definition.source;args[4]=assets::id_text(selected->definition.id);
    }else for(std::size_t i=0;i<args.size();++i){auto converted=utf8(argv[i+1]);if(!converted)return error(converted.error());args[i]=std::move(converted).value();}
    const auto recipe_revision=selected?assets::hex(selected->revision):std::string{};
    const auto index_revision=project?assets::hex(project->revision()):std::string{};
    if(!platform::valid_content_leaf(args[1])||!std::string_view(args[1]).ends_with(".obj"))return error(fail(ErrorCode::InvalidInput,"Worker import requires canonical OBJ leaf"));
    auto id=assets::parse_id(args[4]);if(!id)return error(id.error());
    auto input=platform::make_rooted_files(args[0]),output=platform::make_rooted_files(args[2]);
    if(!input)return error(input.error());if(!output)return error(output.error());
    auto source=input.value()->read(args[1],mesh_pipeline::Limits{}.source_bytes);if(!source)return error(source.error());
    const auto stage_path=std::filesystem::path(argv[project_mode?7:recipe_mode?6:4]);
    if(!stage_path.is_absolute()||!std::filesystem::create_directory(stage_path))
      return error(fail(ErrorCode::InvalidInput,"Worker staging directory must be fresh and absolute"));
    auto staging=platform::make_rooted_files(args[3]);if(!staging)return error(staging.error());
    std::wstring image(32768,L'\0');const auto size=GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));
    if(!size||size>=image.size())return error(fail(ErrorCode::PlatformFailure,"Worker executable discovery failed"));image.resize(size);
    const auto worker=utf8((std::filesystem::path(image).parent_path()/L"race_mesh_import_worker.exe").native());if(!worker)return error(worker.error());
    auto process=platform::process::make_windows_service(1);if(!process)return error(process.error());
    auto job=mesh_import::Job::start(std::move(process).value(),staging.value(),output.value(),worker.value(),args[3],id.value(),source.value(),selected?selected->definition.profile:mesh_pipeline::Profile::Geometry);
    if(!job)return error(job.error());const auto admitted=Clock::now();
    auto completion=job.value()->wait(60000);if(!completion){(void)job.value()->cancel();return error(completion.error());}
    if(completion.value().failure){auto diagnostic=job.value()->stderr_prefix();if(diagnostic&&!diagnostic.value().empty())
      std::fwrite(diagnostic.value().data(),1,diagnostic.value().size(),stderr);}
    const auto finished=Clock::now();auto published=job.value()->publish();if(!published)return error(published.error());
    const auto end=Clock::now();const auto ms=[](auto d){return std::chrono::duration<double,std::milli>(d).count();};
    std::printf("{\"source_commit\":\"%s\",\"authored_sha256\":\"%s\",\"recipe_revision\":\"%s\",\"index_revision\":\"%s\",\"asset\":\"%s\",\"source\":\"%s\",\"leaf\":\"%s\",\"source_bytes\":%zu,\"quiescent\":true,\"admission_ms\":%.4f,\"completion_ms\":%.4f,\"publication_ms\":%.4f}\n",
      build::source_commit,build::authored_input_sha256,recipe_revision.c_str(),index_revision.c_str(),args[4].c_str(),assets::hex(assets::digest(source.value())).c_str(),
      published.value().c_str(),source.value().size(),ms(admitted-begin),ms(finished-admitted),ms(end-finished));
    return 0;
  }catch(const std::bad_alloc&){return error(fail(ErrorCode::Exhausted,"Worker tool allocation"));}
  catch(const std::filesystem::filesystem_error&){return error(fail(ErrorCode::IoFailure,"Worker tool path failure"));}
}
