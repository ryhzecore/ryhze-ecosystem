#include "desktop_mesh_reimport.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <type_traits>

namespace race::desktop {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point begin) noexcept {
  return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
}
bool path(std::string_view text) noexcept {
  return !text.empty()&&text.size()<=4096&&text.find('\0')==std::string_view::npos;
}
Result<void> names(const MeshReimportRequest& request) noexcept {
  if(!request.epoch||!path(request.source_root)||!path(request.product_root)||
      !path(request.checkpoint_root)||!path(request.staging_root))
    return fail(ErrorCode::InvalidInput,"Invalid editor mesh reimport roots or epoch");
  if(request.source_root.capacity()>4096||request.product_root.capacity()>4096||
      request.checkpoint_root.capacity()>4096||request.staging_root.capacity()>4096)
    return fail(ErrorCode::Exhausted,"Editor mesh reimport retained root capacity exceeded");
  return {};
}
std::size_t root_storage(const MeshReimportRequest& request) noexcept {
  return request.source_root.capacity()+1+request.product_root.capacity()+1+
      request.checkpoint_root.capacity()+1+request.staging_root.capacity()+1;
}
Result<std::size_t> view_storage(const ProjectBrowserView& view) noexcept {
  if(!path(view.source.recipe_root)||!content_project::valid_index_leaf(view.source.project_leaf))return fail(ErrorCode::InvalidInput,"Invalid displayed project roots");
  if(view.source.recipe_root.capacity()>4096||view.source.project_leaf.capacity()>96)
    return fail(ErrorCode::Exhausted,"Displayed project retained path capacity exceeded");
  const auto amount=view.project.retained_bytes()+sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+
      view.source.recipe_root.capacity()+1+view.source.project_leaf.capacity()+1;
  if(amount>content_project::maximum_retained_bytes)return fail(ErrorCode::Exhausted,"Displayed project retained capacity exceeded");
  return amount;
}
Result<std::uint64_t> owner_identity() noexcept {
  static std::atomic<std::uint64_t> next{};
  auto prior=next.load(std::memory_order_relaxed);
  for(;;){
    if(prior==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Editor reimport owner identities exhausted");
    if(next.compare_exchange_weak(prior,prior+1,std::memory_order_relaxed))return prior+1;
  }
}
bool quiescent(const platform::process::Snapshot& value) noexcept {
  return value.phase==platform::process::Phase::Completed&&value.stdout_finished&&value.stderr_finished;
}
struct CompletionGuard {
  std::atomic<bool>& done;
  std::atomic<MeshReimportPhase>& phase;
  ~CompletionGuard() noexcept {
    phase.store(MeshReimportPhase::Complete,std::memory_order_release);
    done.store(true,std::memory_order_release);
  }
};
// Enforce actual returned vector capacities too, including lower-layer staging
// and immutable publication readback. Services cannot bypass the declared bound
// by returning a small vector with a large retained allocation.
class BoundedRoot final:public platform::RootedFiles {
  std::shared_ptr<platform::RootedFiles> root_;
public:
  explicit BoundedRoot(std::shared_ptr<platform::RootedFiles> root) noexcept:root_(std::move(root)){}
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override {
    auto read=root_->read(leaf,limit);if(!read)return read.error();
    if(read.value().size()>limit||read.value().capacity()>limit)return fail(ErrorCode::Exhausted,"Editor reimport read capacity exceeded");
    return read;
  }
  Result<void> publish_immutable(std::string_view leaf,std::span<const std::byte> bytes) override {
    return root_->publish_immutable(leaf,bytes);
  }
  Result<void> replace(std::string_view leaf,std::span<const std::byte> bytes,platform::Durability durability) override {
    return root_->replace(leaf,bytes,durability);
  }
};
}
struct MeshReimportTask::Pending {
  std::atomic<bool> done{},cancelled{};
  std::atomic<MeshReimportPhase> phase{MeshReimportPhase::Revalidating};
  std::atomic<std::size_t> source_bytes{};
  MeshReimportCompletion completion;
  std::uint64_t owner_identity{};
  std::shared_ptr<const ProjectBrowserView> project;
  MeshReimportRequest names;
  std::shared_ptr<const MeshReimportCheckpoint> prepared;
};
static_assert(std::is_nothrow_move_constructible_v<MeshReimportCompletion>);
MeshReimportTask::MeshReimportTask(RootFactory roots,WorkerFactory workers)
    :roots_(std::move(roots)),workers_(std::move(workers)) {
  if(!roots_)roots_=[](std::string_view root){return platform::make_rooted_files(root);};
}
MeshReimportTask::~MeshReimportTask() {
  if(worker_.joinable()){
    pending_->cancelled.store(true,std::memory_order_release);
    worker_.join();
  }
}
Result<std::uint64_t> MeshReimportTask::start(std::shared_ptr<const ProjectBrowserView> project,
    assets::AssetId selected,MeshReimportRequest request) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Editor reimport admission requires its owner");
  if(busy())return fail(ErrorCode::Busy,"An editor mesh reimport remains retained");
  const auto begin=Clock::now();auto checked=names(request);if(!checked)return checked.error();
  if(!project||!selected.valid())return fail(ErrorCode::InvalidInput,"Editor reimport requires displayed project and asset");
  auto retained=view_storage(*project);if(!retained)return retained.error();
  auto selected_recipe=project->project.find(selected);if(!selected_recipe)return selected_recipe.error();
  if(selected_recipe.value()->reference.kind!=content_project::Kind::Mesh||
      !std::holds_alternative<mesh_recipe::Snapshot>(selected_recipe.value()->recipe))
    return fail(ErrorCode::InvalidDependency,"Editor mesh reimport requires a mesh recipe");
  if(!workers_)return fail(ErrorCode::Unsupported,"No trusted editor importer composition supplied");
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Editor reimport request IDs exhausted");
  auto identity=owner_identity();if(!identity)return identity.error();
  try {
    auto pending=std::make_unique<Pending>();pending->project=std::move(project);pending->names=std::move(request);
    pending->owner_identity=identity.value();auto& out=pending->completion;
    out.request=sequence_+1;out.epoch=pending->names.epoch;out.selected=selected;
    auto roots=roots_;auto workers=workers_;
    auto running=std::thread([roots=std::move(roots),workers=std::move(workers),state=pending.get()]() mutable noexcept {
      CompletionGuard complete{state->done,state->phase};
      auto worker_roots=std::move(roots);auto worker_factory=std::move(workers);
      auto& out=state->completion;
      const auto cancelled=[&]() noexcept {
        if(!state->cancelled.load(std::memory_order_acquire))return false;
        out.result=fail(ErrorCode::Cancelled,"Editor reimport cancelled while retaining worker ownership");return true;
      };
      const auto set_phase=[&](MeshReimportPhase value) noexcept {state->phase.store(value,std::memory_order_release);};
      try {
        if(cancelled())return;
        auto revalidate_begin=Clock::now();auto recipes=worker_roots(state->project->source.recipe_root);
        if(!recipes){out.result=recipes.error();return;}if(!recipes.value()){out.result=fail(ErrorCode::InvalidInput,"Editor reimport recipe root is empty");return;}
        auto recipe_files=std::make_shared<BoundedRoot>(recipes.value());
        auto fresh=content_project::load(*recipe_files,state->project->source.project_leaf);
        out.revalidate_ms=elapsed(revalidate_begin);if(!fresh){out.result=fresh.error();return;}
        auto selected=fresh.value().find(out.selected);if(!selected){out.result=selected.error();return;}
        auto original=state->project->project.find(out.selected);if(!original){out.result=original.error();return;}
        const auto* selected_mesh=std::get_if<mesh_recipe::Snapshot>(&selected.value()->recipe);
        const auto* original_mesh=std::get_if<mesh_recipe::Snapshot>(&original.value()->recipe);
        if(selected.value()->reference.kind!=content_project::Kind::Mesh||!selected_mesh||!original_mesh){
          out.result=fail(ErrorCode::InvalidDependency,"Editor mesh reimport requires a mesh recipe");return;
        }
        if(fresh.value().revision()!=state->project->project.revision()||
            !std::equal(fresh.value().bytes().begin(),fresh.value().bytes().end(),state->project->project.bytes().begin(),state->project->project.bytes().end())||
            selected_mesh->revision!=original_mesh->revision||selected_mesh->bytes!=original_mesh->bytes){
          out.result=fail(ErrorCode::InvalidDependency,"Displayed project or recipe raw revision changed");return;
        }
        if(cancelled())return;
        set_phase(MeshReimportPhase::ReadingSource);const auto source_begin=Clock::now();
        auto source=worker_roots(state->names.source_root);if(!source){out.result=source.error();return;}
        if(!source.value()){out.result=fail(ErrorCode::InvalidInput,"Editor reimport source root is empty");return;}
        auto source_files=std::make_shared<BoundedRoot>(source.value());
        auto bytes=source_files->read(selected_mesh->definition.source,mesh_pipeline::Limits{}.source_bytes);
        if(!bytes){out.result=bytes.error();return;}if(bytes.value().empty()){out.result=fail(ErrorCode::InvalidInput,"Editor mesh source is empty");return;}
        // One selected-source filesystem read and one task-level digest. The
        // reused Reimport/Job layers defensively rehash their retained copies.
        out.source=assets::digest(bytes.value());out.source_bytes=bytes.value().size();
        state->source_bytes.store(out.source_bytes,std::memory_order_release);out.source_ms=elapsed(source_begin);
        if(cancelled())return;
        auto products=worker_roots(state->names.product_root);if(!products){out.result=products.error();return;}
        auto checkpoints=worker_roots(state->names.checkpoint_root);if(!checkpoints){out.result=checkpoints.error();return;}
        if(!products.value()||!checkpoints.value()){out.result=fail(ErrorCode::InvalidInput,"Editor reimport output root is empty");return;}
        auto product_files=std::make_shared<BoundedRoot>(products.value());auto checkpoint_files=std::make_shared<BoundedRoot>(checkpoints.value());
        set_phase(MeshReimportPhase::Starting);auto prepared=worker_factory(state->owner_identity,state->names);
        if(!prepared){out.result=prepared.error();return;}
        auto& child=prepared.value();
        if(!child.stage||!child.process||!path(child.executable)||!path(child.directory)){
          out.result=fail(ErrorCode::InvalidInput,"Invalid trusted editor reimport worker");return;
        }
        if(child.executable.capacity()>4096||child.directory.capacity()>4096){out.result=fail(ErrorCode::Exhausted,"Editor worker retained path capacity exceeded");return;}
        out.staging_directory=child.directory;
        if(out.staging_directory.capacity()>4096){out.result=fail(ErrorCode::Exhausted,"Editor worker evidence path capacity exceeded");return;}
        if(child.stage==products.value()||child.stage==checkpoints.value()||child.stage==source.value()||child.stage==recipes.value()){
          out.result=fail(ErrorCode::InvalidInput,"Editor worker stage aliases a content root");return;
        }
        if(cancelled())return;
        auto stage_files=std::make_shared<BoundedRoot>(child.stage);
        auto memory=TrackedAllocator::create({assets::mesh::Limits{}.retained_bytes});if(!memory){out.result=memory.error();return;}
        auto owner=mesh_import::Reimport::create(state->owner_identity,out.selected,product_files,memory.value(),{},selected_mesh->definition.profile);
        if(!owner){out.result=owner.error();return;}auto desired=owner.value()->desire(out.source);if(!desired){out.result=desired.error();return;}
        const auto import_begin=Clock::now();
        auto launched=owner.value()->start(desired.value(),std::move(child.process),stage_files,child.executable,child.directory,bytes.value());
        if(!launched){out.result=launched.error();return;}
        set_phase(MeshReimportPhase::Importing);bool stopping{};
        for(;;){
          if(state->cancelled.load(std::memory_order_acquire)&&!stopping){
            stopping=true;set_phase(MeshReimportPhase::Stopping);
            auto stop=owner.value()->invalidate();if(!stop){out.result=stop.error();return;}
          }
          auto process=owner.value()->wait(25);
          if(!process){if(process.error().code==ErrorCode::Timeout)continue;out.result=process.error();return;}
          if(quiescent(process.value()))break;
          // A service may report completed process with streams still live.
          // Retain ownership and avoid spinning while awaiting that boundary.
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        out.import_ms=elapsed(import_begin);
        if(stopping||cancelled()){
          auto reaped=owner.value()->reap();if(!reaped){out.result=reaped.error();return;}
          out.result=fail(ErrorCode::Cancelled,"Editor reimport cancelled after process and stream quiescence");return;
        }
        set_phase(MeshReimportPhase::Publishing);const auto publish_begin=Clock::now();
        auto handle=owner.value()->finish(desired.value());out.cook_publish_ms=elapsed(publish_begin);
        if(!handle){out.result=handle.error();return;}
        if(cancelled())return;
        auto lease=owner.value()->acquire(handle.value());if(!lease){out.result=lease.error();return;}
        out.product_bytes=lease.value()->view().bytes().size();
        set_phase(MeshReimportPhase::Capturing);const auto capture_begin=Clock::now();
        auto captured=mesh_checkpoint::capture(state->project->project,std::move(lease).value(),*product_files,*checkpoint_files);
        out.capture_ms=elapsed(capture_begin);if(!captured){out.result=captured.error();return;}
        if(cancelled())return;
        if(captured.value().leaf.size()!=69){out.result=fail(ErrorCode::InvalidInput,"Editor checkpoint leaf length differs");return;}
        std::string bounded_leaf(captured.value().leaf);
        if(bounded_leaf.capacity()>96){out.result=fail(ErrorCode::Exhausted,"Editor checkpoint retained leaf capacity exceeded");return;}
        captured.value().leaf.swap(bounded_leaf);
        auto stored=view_storage(*state->project);if(!stored){out.result=stored.error();return;}
        out.retained_bytes=sizeof(MeshReimportCheckpoint)+stored.value()+root_storage(state->names)+captured.value().leaf.capacity()+1;
        if(out.retained_bytes>maximum_checkpoint_retained_bytes){out.result=fail(ErrorCode::Exhausted,"Editor selected checkpoint retained capacity exceeded");return;}
        state->prepared=std::make_shared<MeshReimportCheckpoint>(state->project,std::move(state->names),std::move(captured).value());
      }catch(const std::bad_alloc&) {out.result=fail(ErrorCode::Exhausted,"Background editor reimport allocation failed");}
       catch(...) {out.result=fail(ErrorCode::PlatformFailure,"Background editor reimport failed");}
    });
    pending_=std::move(pending);worker_=std::move(running);
    pending_->completion.admission_ms=elapsed(begin);return ++sequence_;
  }catch(const std::bad_alloc&) {return fail(ErrorCode::Exhausted,"Editor reimport admission allocation failed");}
   catch(...) {return fail(ErrorCode::PlatformFailure,"Editor reimport worker could not start");}
}
Result<void> MeshReimportTask::cancel() {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Editor reimport cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No editor reimport remains retained");
  pending_->cancelled.store(true,std::memory_order_release);return {};
}
Result<MeshReimportProgress> MeshReimportTask::progress() const noexcept {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Editor reimport progress requires its owner");
  if(!busy())return MeshReimportProgress{};
  return MeshReimportProgress{pending_->phase.load(std::memory_order_acquire),pending_->source_bytes.load(std::memory_order_acquire),pending_->cancelled.load(std::memory_order_acquire)};
}
Result<std::optional<MeshReimportCompletion>> MeshReimportTask::poll(std::shared_ptr<const ProjectBrowserView> current,
    assets::AssetId selected,std::uint64_t epoch,std::shared_ptr<const MeshReimportCheckpoint>& checkpoint) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Editor reimport completion requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))return std::optional<MeshReimportCompletion>{};
  try {worker_.join();}catch(...) {return fail(ErrorCode::PlatformFailure,"Editor reimport worker join failed; ownership retained");}
  auto pending=std::move(pending_);auto& out=pending->completion;const auto begin=Clock::now();
  if(pending->cancelled.load(std::memory_order_acquire))out.result=fail(ErrorCode::Cancelled,"Editor checkpoint selection cancelled after worker quiescence");
  else if(epoch!=out.epoch||current!=pending->project||selected!=out.selected)
    out.result=fail(ErrorCode::StaleHandle,"Displayed project epoch or selected asset changed while reimporting");
  else if(out.result){
    if(!pending->prepared)out.result=fail(ErrorCode::Invariant,"Editor reimport completed without a verified checkpoint");
    else {checkpoint.swap(pending->prepared);out.changed=true;}
  }
  out.publication_ms=elapsed(begin);return std::optional<MeshReimportCompletion>{std::move(out)};
}
} // namespace race::desktop
