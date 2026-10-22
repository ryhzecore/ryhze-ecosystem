#include "desktop_surface_reimport.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

namespace race::desktop {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point begin){return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();}
bool path(const std::string& value){return !value.empty()&&value.size()<=4096&&value.capacity()<=4096&&value.find('\0')==std::string::npos;}
Result<void> valid_view(const ProjectBrowserView& view){
  if(!path(view.source.recipe_root)||!content_project::valid_index_leaf(view.source.project_leaf)||view.source.project_leaf.capacity()>96)
    return fail(ErrorCode::InvalidInput,"Invalid retained project paths");
  if(view.project.retained_bytes()+sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+view.source.recipe_root.capacity()+view.source.project_leaf.capacity()+2>content_project::maximum_retained_bytes)
    return fail(ErrorCode::Exhausted,"Retained project capacity exceeded");
  return {};
}
struct Complete {std::atomic<bool>& done;std::atomic<SurfaceReimportPhase>& phase;~Complete(){phase.store(SurfaceReimportPhase::Complete,std::memory_order_release);done.store(true,std::memory_order_release);}};
}
struct SurfaceReimportTask::Pending {
  std::atomic<bool> done{},cancelled{};
  std::atomic<SurfaceReimportPhase> phase{SurfaceReimportPhase::Revalidating};
  std::atomic<std::size_t> source_bytes{};
  SurfaceReimportCompletion completion;
  std::shared_ptr<const ProjectBrowserView> project;
  SurfaceReimportRequest roots;
  std::shared_ptr<const SurfaceReimportCheckpoint> prepared;
};
SurfaceReimportTask::SurfaceReimportTask(RootFactory roots):roots_(std::move(roots)){
  if(!roots_)roots_=[](std::string_view name){return platform::make_rooted_files(name);};
}
SurfaceReimportTask::~SurfaceReimportTask(){if(worker_.joinable()){pending_->cancelled.store(true,std::memory_order_release);worker_.join();}}
Result<std::uint64_t> SurfaceReimportTask::start(std::shared_ptr<const ProjectBrowserView> project,assets::AssetId selected,SurfaceReimportRequest names){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Surface import requires its owner");
  if(busy())return fail(ErrorCode::Busy,"Surface import remains retained");
  if(!project||!selected.valid()||!names.epoch||!path(names.source_root)||!path(names.product_root)||!path(names.checkpoint_root))
    return fail(ErrorCode::InvalidInput,"Invalid surface import roots or selection");
  auto checked=valid_view(*project);if(!checked)return checked.error();
  auto entry=project->project.find(selected);if(!entry)return entry.error();
  if(entry.value()->reference.kind==content_project::Kind::Mesh||!std::holds_alternative<surface_recipe::Snapshot>(entry.value()->recipe))
    return fail(ErrorCode::InvalidInput,"Surface import requires a texture or material recipe");
  if(names.source_root==names.product_root||names.source_root==names.checkpoint_root||names.product_root==names.checkpoint_root||
      project->source.recipe_root==names.source_root||project->source.recipe_root==names.product_root||project->source.recipe_root==names.checkpoint_root)
    return fail(ErrorCode::InvalidInput,"Surface import requires separate source, recipe, product and checkpoint roots");
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Surface import request IDs exhausted");
  try{
    const auto begin=Clock::now();auto pending=std::make_unique<Pending>();pending->project=std::move(project);pending->roots=std::move(names);
    pending->completion.request=sequence_+1;pending->completion.epoch=pending->roots.epoch;pending->completion.selected=selected;
    auto roots=roots_;pending->completion.admission_ms=elapsed(begin);
    std::thread running([state=pending.get(),roots=std::move(roots)]() mutable noexcept {
      Complete completed{state->done,state->phase};auto factory=std::move(roots);auto& out=state->completion;
      const auto cancelled=[&](){if(!state->cancelled.load(std::memory_order_acquire))return false;out.result=fail(ErrorCode::Cancelled,"Surface import cancelled; immutable files retained");return true;};
      const auto work=[&]()->Result<void>{
        if(cancelled())return out.result;auto begin=Clock::now();
        auto recipes=factory(state->project->source.recipe_root);if(!recipes)return recipes.error();if(!recipes.value())return fail(ErrorCode::InvalidInput,"Empty recipe root");
        auto project=content_project::load(*recipes.value(),state->project->source.project_leaf);if(!project)return project.error();
        if(project.value().revision()!=state->project->project.revision()||!std::equal(project.value().bytes().begin(),project.value().bytes().end(),state->project->project.bytes().begin(),state->project->project.bytes().end()))
          return fail(ErrorCode::InvalidDependency,"Displayed project changed before surface import");
        auto found=project.value().find(out.selected);if(!found)return found.error();
        const auto* recipe=std::get_if<surface_recipe::Snapshot>(&found.value()->recipe);if(!recipe)return fail(ErrorCode::InvalidDependency,"Surface recipe kind changed");
        auto source=factory(state->roots.source_root);if(!source)return source.error();
        auto products=factory(state->roots.product_root);if(!products)return products.error();
        auto checkpoints=factory(state->roots.checkpoint_root);if(!checkpoints)return checkpoints.error();
        if(!source.value()||!products.value()||!checkpoints.value())return fail(ErrorCode::InvalidInput,"Empty surface root capability");
        out.revalidate_ms=elapsed(begin);if(cancelled())return out.result;
        state->phase.store(SurfaceReimportPhase::ReadingSource,std::memory_order_release);begin=Clock::now();
        auto info=surface_pipeline::profile_info(recipe->definition.profile);if(!info)return info.error();
        const auto limit=info.value().material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes;
        auto bytes=source.value()->read(recipe->definition.source,limit);if(!bytes)return bytes.error();
        if(bytes.value().size()>limit||bytes.value().capacity()>limit)return fail(ErrorCode::Exhausted,"Surface source capacity limit");
        out.source_bytes=bytes.value().size();out.source=assets::digest(bytes.value());state->source_bytes.store(out.source_bytes,std::memory_order_release);out.source_ms=elapsed(begin);
        if(cancelled())return out.result;state->phase.store(SurfaceReimportPhase::Building,std::memory_order_release);begin=Clock::now();
        auto built=surface_recipe::build(*recipe,bytes.value(),products.value().get());if(!built)return built.error();
        out.build_ms=elapsed(begin);out.product_bytes=built.value().cooked.size();
        if(cancelled())return out.result;state->phase.store(SurfaceReimportPhase::Publishing,std::memory_order_release);begin=Clock::now();
        auto imported=surface_pipeline::publish(*products.value(),built.value().imported);if(!imported)return imported.error();
        if(cancelled())return out.result;
        auto cooked=surface_pipeline::publish(*products.value(),built.value().cooked);if(!cooked)return cooked.error();out.publish_ms=elapsed(begin);
        if(cancelled())return out.result;state->phase.store(SurfaceReimportPhase::Capturing,std::memory_order_release);begin=Clock::now();
        auto captured=surface_checkpoint::capture(project.value(),built.value().cooked,*products.value(),*checkpoints.value());if(!captured)return captured.error();out.capture_ms=elapsed(begin);
        if(cancelled())return out.result;
        auto prepared=std::make_shared<SurfaceReimportCheckpoint>(state->project,state->roots,std::move(captured).value());
        out.retained_bytes=sizeof(SurfaceReimportCheckpoint)+prepared->roots.source_root.capacity()+prepared->roots.product_root.capacity()+prepared->roots.checkpoint_root.capacity()+prepared->published.leaf.capacity()+
            sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+prepared->project->project.retained_bytes()+prepared->project->source.recipe_root.capacity()+prepared->project->source.project_leaf.capacity()+6;
        if(out.retained_bytes>maximum_surface_checkpoint_retained_bytes)return fail(ErrorCode::Exhausted,"Surface checkpoint retained capacity limit");
        state->prepared=std::move(prepared);return {};
      };
      try{out.result=work();}catch(const std::bad_alloc&){out.result=fail(ErrorCode::Exhausted,"Surface import allocation failed");}catch(...){out.result=fail(ErrorCode::PlatformFailure,"Surface import failed");}
    });
    pending_=std::move(pending);worker_=std::move(running);return ++sequence_;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface import admission allocation");}catch(...){return fail(ErrorCode::PlatformFailure,"Surface import worker could not start");}
}
Result<void> SurfaceReimportTask::cancel(){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Surface cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No surface import retained");pending_->cancelled.store(true,std::memory_order_release);return {};
}
Result<SurfaceReimportProgress> SurfaceReimportTask::progress() const noexcept{
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Surface progress requires its owner");
  if(!busy())return SurfaceReimportProgress{};
  return SurfaceReimportProgress{pending_->phase.load(std::memory_order_acquire),pending_->source_bytes.load(std::memory_order_acquire),pending_->cancelled.load(std::memory_order_acquire)};
}
Result<std::optional<SurfaceReimportCompletion>> SurfaceReimportTask::poll(std::shared_ptr<const ProjectBrowserView> current,assets::AssetId selected,std::uint64_t epoch,std::shared_ptr<const SurfaceReimportCheckpoint>& checkpoint){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Surface publication requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))return std::optional<SurfaceReimportCompletion>{};
  try{worker_.join();}catch(...){return fail(ErrorCode::PlatformFailure,"Surface join failed; ownership retained");}
  auto pending=std::move(pending_);auto& out=pending->completion;const auto begin=Clock::now();
  if(pending->cancelled.load(std::memory_order_acquire))out.result=fail(ErrorCode::Cancelled,"Surface publication cancelled after quiescence");
  else if(current!=pending->project||epoch!=out.epoch||selected!=out.selected)out.result=fail(ErrorCode::StaleHandle,"Surface selection or project changed");
  else if(out.result){if(!pending->prepared)out.result=fail(ErrorCode::Invariant,"Surface checkpoint missing after completion");else{checkpoint.swap(pending->prepared);out.changed=true;}}
  out.publication_ms=elapsed(begin);return std::optional<SurfaceReimportCompletion>{std::move(out)};
}
} // namespace race::desktop
