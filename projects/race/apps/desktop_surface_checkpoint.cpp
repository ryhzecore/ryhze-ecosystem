#include "desktop_surface_checkpoint.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

namespace race::desktop {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
bool path(const std::string& value){return !value.empty()&&value.capacity()<=4096&&value.find('\0')==std::string::npos;}
struct Complete {std::atomic<bool>& done;~Complete(){done.store(true,std::memory_order_release);}};
}
struct SurfaceCheckpointTask::Pending {
  std::atomic<bool> done{};
  PersistenceStamp stamp;
  ObjectRow target;
  SurfaceCheckpointRequest names;
  SurfaceCheckpointCompletion completion;
  std::shared_ptr<const ProjectBrowserView> project;
  std::shared_ptr<const MeshSet> prior,prepared;
};
SurfaceCheckpointTask::SurfaceCheckpointTask(RootFactory roots):roots_(std::move(roots)){
  if(!roots_)roots_=[](std::string_view name){return platform::make_rooted_files(name);};
}
SurfaceCheckpointTask::~SurfaceCheckpointTask(){if(worker_.joinable())worker_.join();}
Result<std::uint64_t> SurfaceCheckpointTask::start(const Document& document,std::shared_ptr<const MeshSet> prior,PersistentId selection,
    std::shared_ptr<const ProjectBrowserView> project,SurfaceCheckpointRequest names){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Surface adoption requires its owner");
  if(busy())return fail(ErrorCode::Busy,"Surface adoption remains retained");
  if(document.playing())return fail(ErrorCode::Busy,"Stop Play before material adoption");
  if(!prior||!prior->files()||!project||!selection.valid()||!names.asset.valid()||!names.epoch||!path(names.checkpoint_root)||
      !platform::valid_content_leaf(names.checkpoint_leaf)||names.checkpoint_leaf.capacity()>96||names.checkpoint_leaf.size()!=69||!names.checkpoint_leaf.ends_with(".json")||
      !path(project->source.recipe_root)||project->source.project_leaf.capacity()>96||!platform::valid_content_leaf(project->source.project_leaf))
    return fail(ErrorCode::InvalidInput,"Invalid material checkpoint selection or paths");
  if(project->project.retained_bytes()+sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+project->source.recipe_root.capacity()+project->source.project_leaf.capacity()+2>content_project::maximum_retained_bytes)
    return fail(ErrorCode::Exhausted,"Material checkpoint project capacity limit");
  auto entry=project->project.find(names.asset);if(!entry)return entry.error();
  if(entry.value()->reference.kind!=content_project::Kind::Material)return fail(ErrorCode::InvalidInput,"Only material checkpoints can be assigned to objects");
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Material adoption request IDs exhausted");
  try{
    const auto begin=Clock::now();auto rows=document.objects();if(!rows)return rows.error();
    auto row=std::find_if(rows.value().begin(),rows.value().end(),[&](const auto& r){return r.id==selection;});
    if(row==rows.value().end()||!row->visible)return fail(ErrorCode::InvalidInput,"Select a visible Scene object for material adoption");
    auto pending=std::make_unique<Pending>();pending->stamp=document.persistence_stamp();pending->target=*row;pending->prior=std::move(prior);pending->project=std::move(project);pending->names=std::move(names);
    auto& out=pending->completion;out.request=sequence_+1;out.epoch=pending->names.epoch;out.selected=selection;out.asset=pending->names.asset;out.previous=row->material;
    auto roots=roots_;out.snapshot_ms=elapsed(begin);
    std::thread running([state=pending.get(),roots=std::move(roots)]() mutable noexcept {
      Complete complete{state->done};auto factory=std::move(roots);auto& out=state->completion;
      const auto prepare=[&]()->Result<void>{
        auto begin=Clock::now();auto recipes=factory(state->project->source.recipe_root);if(!recipes)return recipes.error();
        auto checkpoints=factory(state->names.checkpoint_root);if(!checkpoints)return checkpoints.error();
        if(!recipes.value()||!checkpoints.value())return fail(ErrorCode::InvalidInput,"Empty material checkpoint root");
        auto project=content_project::load(*recipes.value(),state->project->source.project_leaf);if(!project)return project.error();
        if(project.value().revision()!=state->project->project.revision()||!std::equal(project.value().bytes().begin(),project.value().bytes().end(),state->project->project.bytes().begin(),state->project->project.bytes().end()))
          return fail(ErrorCode::InvalidDependency,"Displayed content project changed before adoption");
        auto loaded=surface_checkpoint::load(*checkpoints.value(),state->names.checkpoint_leaf,project.value(),*state->prior->files());if(!loaded)return loaded.error();
        const auto& definition=loaded.value().definition();
        if(definition.kind!=content_project::Kind::Material||definition.id!=out.asset)return fail(ErrorCode::InvalidDependency,"Checkpoint differs from the selected Content material");
        out.replacement={definition.id,definition.cooked_revision};out.product_bytes=loaded.value().product().size();out.texture_bytes=loaded.value().texture().size();out.load_ms=elapsed(begin);begin=Clock::now();
        auto prepared=MeshSet::extend_material(state->prior,loaded.value().product());if(!prepared)return prepared.error();
        auto target=state->target;target.material=out.replacement;
        auto draw=mesh_draw(target,prepared.value().get());if(!draw)return draw.error();
        state->prepared=std::move(prepared).value();out.prepare_ms=elapsed(begin);return {};
      };
      try{out.result=prepare();}catch(const std::bad_alloc&){out.result=fail(ErrorCode::Exhausted,"Material checkpoint preparation allocation");}catch(...){out.result=fail(ErrorCode::PlatformFailure,"Material checkpoint preparation failed");}
    });
    pending_=std::move(pending);worker_=std::move(running);cancelled_=false;return ++sequence_;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Material checkpoint admission allocation");}catch(...){return fail(ErrorCode::PlatformFailure,"Material checkpoint worker could not start");}
}
Result<void> SurfaceCheckpointTask::cancel(){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Material adoption cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No material checkpoint retained");cancelled_=true;return {};
}
Result<std::optional<SurfaceCheckpointCompletion>> SurfaceCheckpointTask::poll(Document& document,std::shared_ptr<const MeshSet>& current,PersistentId selection,
    std::shared_ptr<const ProjectBrowserView> project,assets::AssetId asset,std::uint64_t epoch,bool allow_publication){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Material adoption publication requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))return std::optional<SurfaceCheckpointCompletion>{};
  try{worker_.join();}catch(...){return fail(ErrorCode::PlatformFailure,"Material adoption join failed; ownership retained");}
  auto pending=std::move(pending_);auto& out=pending->completion;const auto begin=Clock::now();
  if(cancelled_||!allow_publication)out.result=fail(ErrorCode::Cancelled,"Material adoption cancelled after worker quiescence");
  else if(!document.current(pending->stamp)||document.playing()||selection!=out.selected||current!=pending->prior||project!=pending->project||asset!=out.asset||epoch!=out.epoch)
    out.result=fail(ErrorCode::StaleHandle,"Scene or Content context changed during material adoption");
  else if(out.result){
    if(!pending->prepared)out.result=fail(ErrorCode::Invariant,"Material checkpoint completed without dependencies");
    else if(out.previous!=out.replacement){
      // Document validates and commits history before the no-throw cache swap.
      try{out.result=document.assign_material(out.selected,out.replacement);if(out.result){current.swap(pending->prepared);out.changed=true;}}
      catch(const std::bad_alloc&){out.result=fail(ErrorCode::Exhausted,"Material adoption transaction allocation");}
      catch(...){out.result=fail(ErrorCode::PlatformFailure,"Material adoption transaction failed");}
    }
  }
  out.commit_ms=elapsed(begin);return std::optional<SurfaceCheckpointCompletion>{std::move(out)};
}
} // namespace race::desktop
