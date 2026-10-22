#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mesh_watch.hpp"
#include "mesh_watch_cli.hpp"
#include "mesh_recipe.hpp"
#include "mesh_project.hpp"
#include "mesh_checkpoint.hpp"
#include "../src/platform/windows_root_access.hpp"
#include "race/build_identity.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace race::mesh_import {
namespace {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
Result<std::string> utf8(std::wstring_view value){
  if(value.empty()||value.size()>32767)return fail(ErrorCode::InvalidInput,"Watch path length");
  const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  if(n<=0)return fail(ErrorCode::InvalidInput,"Watch argument encoding");
  std::string out(static_cast<std::size_t>(n),'\0');
  if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n,nullptr,nullptr)!=n)
    return fail(ErrorCode::PlatformFailure,"Watch argument conversion");return out;
}
bool inside(std::wstring root,std::wstring_view path){
  while(root.size()>1&&root.back()==L'\\')root.pop_back();
  return path.size()>=root.size()&&CompareStringOrdinal(root.data(),static_cast<int>(root.size()),path.data(),static_cast<int>(root.size()),TRUE)==CSTR_EQUAL&&
    (path.size()==root.size()||path[root.size()]==L'\\');
}
Result<std::uint32_t> number(std::string_view text,std::uint32_t max){
  std::uint32_t value{};auto result=std::from_chars(text.data(),text.data()+text.size(),value);
  if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size()||!value||value>max)return fail(ErrorCode::InvalidInput,"Watch numeric limit");return value;
}
class Factory final:public WorkerFactory {
  std::shared_ptr<platform::RootedFiles> pin_;
  fs::path base_;
  std::string executable_;
public:
  Factory(std::shared_ptr<platform::RootedFiles> pin,fs::path base,std::string executable)
    :pin_(std::move(pin)),base_(std::move(base)),executable_(std::move(executable)){}
  Result<PreparedWorker> prepare(std::uint32_t attempt) override {
    try{
      const auto path=base_/(L"attempt-"+std::to_wstring(attempt));
      if(!fs::create_directory(path))return fail(ErrorCode::Duplicate,"Watch stage already exists");
      auto directory=utf8(path.native());if(!directory)return directory.error();
      auto stage=platform::make_rooted_files(directory.value());if(!stage)return stage.error();
      auto process=platform::process::make_windows_service(attempt);if(!process)return process.error();
      return PreparedWorker{stage.value(),executable_,directory.value(),std::move(process).value()};
    }catch(const fs::filesystem_error&){return fail(ErrorCode::IoFailure,"Watch stage creation failure");}
  }
};
int error(Error e){std::fprintf(stderr,"mesh_watch_error code=%u reason=%s\n",unsigned(e.code),e.message);return 1;}
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
// Fixed count and record bytes. Status samples can coalesce transitions within
// one poll; events and counters expose that fact. One record is reserved for stop.
bool receipt(const WatchStatus& s,double elapsed,double maximum,bool quota,std::string_view recipe,std::string_view asset,std::string_view index){
  std::array<char,1024> text{};
  const auto source=assets::hex(s.source),revision=assets::hex(s.current_revision);
  const int count=std::snprintf(text.data(),text.size(),
    "{\"authored_sha256\":\"%s\",\"recipe_revision\":\"%.*s\",\"index_revision\":\"%.*s\",\"asset\":\"%.*s\",\"phase\":%u,\"outcome\":%u,\"events\":%llu,\"observed\":%llu,\"desired\":%llu,\"attempts\":%u,\"reads\":%llu,\"read_failures\":%u,\"superseded\":%llu,\"unchanged\":%llu,\"worker_retained\":%s,\"source\":\"%s\",\"current_revision\":\"%s\",\"retained_bytes\":%zu,\"pending_source_bytes\":%zu,\"last_error\":%d,\"terminal_error\":%d,\"receipt_limit\":%s,\"elapsed_ms\":%.4f,\"max_poll_ms\":%.4f}\n",
    build::authored_input_sha256,static_cast<int>(recipe.size()),recipe.data(),static_cast<int>(index.size()),index.data(),static_cast<int>(asset.size()),asset.data(),unsigned(s.phase),unsigned(s.outcome),static_cast<unsigned long long>(s.events),static_cast<unsigned long long>(s.observed.generation),
    static_cast<unsigned long long>(s.desired_generation),s.attempts,static_cast<unsigned long long>(s.source_reads),s.read_failures,
    static_cast<unsigned long long>(s.superseded),static_cast<unsigned long long>(s.unchanged),s.worker_retained?"true":"false",source.c_str(),revision.c_str(),
    s.retained_bytes,s.pending_source_bytes,s.last_error?int(s.last_error->code):-1,s.terminal_error?int(s.terminal_error->code):-1,quota?"true":"false",elapsed,maximum);
  if(count<0||static_cast<std::size_t>(count)>=text.size())return false;
  return std::fwrite(text.data(),1,static_cast<std::size_t>(count),stdout)==static_cast<std::size_t>(count)&&std::fflush(stdout)==0;
}
}
int watch_tool(int argc,wchar_t** argv){
  const bool recipe_mode=argc>=2&&std::wstring_view(argv[1])==L"--watch-recipe";
  const bool project_mode=argc>=2&&std::wstring_view(argv[1])==L"--watch-project";
  const int shift=project_mode?1:0;
  const int required=9+shift;
  if(argc<required||argc>required+(project_mode?6:1)){std::fprintf(stderr,"usage: race_mesh_import_tool --watch SOURCE_ROOT source.obj OUTPUT_ROOT FRESH_STAGE_ROOT ID DURATION_MS MAX_IMPORTS [--coalesce]\n"
    "       race_mesh_import_tool --watch-recipe RECIPE_ROOT recipe.json SOURCE_ROOT OUTPUT_ROOT FRESH_STAGE_ROOT DURATION_MS MAX_IMPORTS [--coalesce]\n"
    "       race_mesh_import_tool --watch-project RECIPE_ROOT project.raceproject ID SOURCE_ROOT OUTPUT_ROOT FRESH_STAGE_ROOT DURATION_MS MAX_IMPORTS [--coalesce] [--checkpoint ROOT] [--resume-checkpoint ROOT LEAF]\n");return 2;}
  try{
    bool coalesce{};std::optional<std::string> checkpoint_root;
    std::optional<std::pair<std::string,std::string>> resume;
    for(int n=required;n<argc;){
      const std::wstring_view option(argv[n++]);
      if(option==L"--coalesce"&&!coalesce)coalesce=true;
      else if(option==L"--checkpoint"&&project_mode&&!checkpoint_root&&n<argc){auto root=utf8(argv[n++]);if(!root)return error(root.error());checkpoint_root=std::move(root).value();}
      else if(option==L"--resume-checkpoint"&&project_mode&&!resume&&n+1<argc){
        auto root=utf8(argv[n++]),leaf=utf8(argv[n++]);if(!root)return error(root.error());if(!leaf)return error(leaf.error());resume=std::pair{std::move(root).value(),std::move(leaf).value()};
      }else return error(fail(ErrorCode::InvalidInput,"Unsupported, duplicate or incomplete watch option"));
    }
    std::array<std::string,7> args;
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
      for(auto pair:{std::pair{0U,4U},std::pair{2U,5U},std::pair{3U,6U},std::pair{5U,7U},std::pair{6U,8U}}){auto converted=utf8(argv[pair.second+shift]);if(!converted)return error(converted.error());args[pair.first]=std::move(converted).value();}
      args[1]=selected->definition.source;args[4]=assets::id_text(selected->definition.id);
    }else for(std::size_t n=0;n<args.size();++n){auto converted=utf8(argv[n+2]);if(!converted)return error(converted.error());args[n]=std::move(converted).value();}
    const auto recipe_revision=selected?assets::hex(selected->revision):std::string{};
    const auto index_revision=project?assets::hex(project->revision()):std::string{};
    auto duration=number(args[5],60000),maximum=number(args[6],64);if(!duration)return error(duration.error());if(!maximum)return error(maximum.error());
    auto id=assets::parse_id(args[4]);if(!id)return error(id.error());
    auto source=platform::make_rooted_files(args[0]),output=platform::make_rooted_files(args[2]);if(!source)return error(source.error());if(!output)return error(output.error());
    auto source_path=platform::windows_pinned_root_path(source.value()),output_path=platform::windows_pinned_root_path(output.value());
    if(!source_path)return error(source_path.error());if(!output_path)return error(output_path.error());
    std::shared_ptr<platform::RootedFiles> checkpoints;
    if(checkpoint_root){
      auto files=platform::make_rooted_files(*checkpoint_root);if(!files)return error(files.error());checkpoints=std::move(files).value();
      auto path=platform::windows_pinned_root_path(checkpoints);if(!path)return error(path.error());
      if(inside(source_path.value(),path.value()))return error(fail(ErrorCode::InvalidInput,"Checkpoint output must be outside watched source"));
    }
    auto allocator=TrackedAllocator::create({64*1024*1024});if(!allocator)return error(allocator.error());
    auto reimport=Reimport::create(1,id.value(),output.value(),allocator.value(),{},selected?selected->definition.profile:mesh_pipeline::Profile::Geometry);if(!reimport)return error(reimport.error());
    if(resume){
      auto files=platform::make_rooted_files(resume->first);if(!files)return error(files.error());
      auto loaded=mesh_checkpoint::load(*files.value(),resume->second,*project,*output.value());if(!loaded)return error(loaded.error());
      if(loaded.value().definition().id!=id.value())return error(fail(ErrorCode::InvalidDependency,"Checkpoint selection differs from watch asset"));
      auto restored=reimport.value()->restore_initial(loaded.value().product());if(!restored)return error(restored.error());
      // The Store owns a validated copy; no worker uses this temporary readback.
    }
    const auto stage_path=fs::path(argv[project_mode?7:recipe_mode?6:5]);
    if(!stage_path.is_absolute()||stage_path.filename().empty()||stage_path.filename()==L"."||stage_path.filename()==L"..")return error(fail(ErrorCode::InvalidInput,"Watch staging must be fresh and absolute"));
    auto parent_name=utf8(stage_path.parent_path().native());if(!parent_name)return error(parent_name.error());
    auto parent=platform::make_rooted_files(parent_name.value());if(!parent)return error(parent.error());
    auto parent_path=platform::windows_pinned_root_path(parent.value());if(!parent_path)return error(parent_path.error());
    const auto candidate=(fs::path(parent_path.value())/stage_path.filename()).native();
    if(inside(source_path.value(),output_path.value())||inside(source_path.value(),candidate))return error(fail(ErrorCode::InvalidInput,"Watch output and staging must be outside source"));
    if(!fs::create_directory(stage_path))return error(fail(ErrorCode::Duplicate,"Watch staging is not fresh"));
    auto stage=platform::make_rooted_files(args[3]);if(!stage)return error(stage.error());
    auto actual=platform::windows_pinned_root_path(stage.value());if(!actual)return error(actual.error());
    if(inside(source_path.value(),actual.value()))return error(fail(ErrorCode::InvalidInput,"Watch stage lies inside source"));
    std::wstring image(32768,L'\0');const auto count=GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));
    if(!count||count>=image.size())return error(fail(ErrorCode::PlatformFailure,"Watch worker discovery"));image.resize(count);
    auto worker=utf8((fs::path(image).parent_path()/L"race_mesh_import_worker.exe").native());if(!worker)return error(worker.error());
    auto watch=platform::directory_watch::make_windows_service(source.value(),1);if(!watch)return error(watch.error());
    auto made=Watch::create(source.value(),args[1],std::move(watch).value(),std::make_unique<Factory>(stage.value(),fs::path(actual.value()),worker.value()),
      std::move(reimport).value(),{maximum.value(),3,coalesce});if(!made)return error(made.error());auto owner=std::move(made).value();
    const unsigned event_records=checkpoints?126U:127U; // terminal + optional checkpoint stay within 128 lines
    const auto begin=Clock::now();std::uint64_t last_event{};unsigned records{};bool quota{},write_failed{};double max_poll{};
    for(;;){
      if(ms(Clock::now()-begin)>=duration.value()||quota||write_failed)(void)owner->request_stop();
      const auto start=Clock::now();auto polled=owner->poll();max_poll=std::max(max_poll,ms(Clock::now()-start));if(!polled)return error(polled.error());
      const auto& s=polled.value();
      if(s.phase==WatchPhase::Stopped){
        if(!write_failed&&!receipt(s,ms(Clock::now()-begin),max_poll,quota,recipe_revision,args[4],index_revision))write_failed=true;
        if(s.terminal_error||quota||write_failed)return 1;
        if(checkpoints){
          auto current=owner->current();if(!current)return error(current.error());auto lease=owner->acquire(current.value());if(!lease)return error(lease.error());
          const auto started=Clock::now();auto captured=mesh_checkpoint::capture(*project,std::move(lease).value(),*output.value(),*checkpoints);if(!captured)return error(captured.error());
          const auto& item=captured.value();std::array<char,768> text{};
          const int bytes=std::snprintf(text.data(),text.size(),"{\"checkpoint_revision\":\"%s\",\"leaf\":\"%s\",\"asset\":\"%s\",\"cooked_revision\":\"%s\",\"source\":\"%s\",\"capture_ms\":%.4f}\n",
            assets::hex(item.revision).c_str(),item.leaf.c_str(),assets::id_text(item.definition.id).c_str(),assets::hex(item.definition.cooked_revision).c_str(),assets::hex(item.definition.source).c_str(),ms(Clock::now()-started));
          if(bytes<0||static_cast<std::size_t>(bytes)>=text.size()||std::fwrite(text.data(),1,static_cast<std::size_t>(bytes),stdout)!=static_cast<std::size_t>(bytes)||std::fflush(stdout)!=0)return error(fail(ErrorCode::IoFailure,"Checkpoint receipt output failed"));
        }
        return 0;
      }
      if(s.events!=last_event&&records<event_records&&!write_failed){
        last_event=s.events;++records;if(!receipt(s,ms(Clock::now()-begin),max_poll,quota,recipe_revision,args[4],index_revision))write_failed=true;
        if(records==event_records)quota=true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }catch(const std::bad_alloc&){return error(fail(ErrorCode::Exhausted,"Watch tool allocation"));}
  catch(const fs::filesystem_error&){return error(fail(ErrorCode::IoFailure,"Watch tool filesystem failure"));}
}
} // namespace race::mesh_import
