#include "desktop_mesh_checkpoint.hpp"
#include "content_mesh_checkpoint.hpp"
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
bool root_name(std::string_view value) noexcept {
  return !value.empty()&&value.size()<=4096&&value.find('\0')==std::string_view::npos;
}
bool request_names(const MeshCheckpointRequest& value) noexcept {
  return root_name(value.recipe_root)&&root_name(value.checkpoint_root)&&
      content_project::valid_index_leaf(value.project_leaf)&&
      platform::valid_content_leaf(value.checkpoint_leaf)&&value.checkpoint_leaf.size()==69&&
      value.checkpoint_leaf.ends_with(".json")&&
      std::all_of(value.checkpoint_leaf.begin(),value.checkpoint_leaf.begin()+64,
          [](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
struct CompletionGuard {
  std::atomic<bool>& done;
  ~CompletionGuard() noexcept { done.store(true,std::memory_order_release); }
};
}
struct MeshCheckpointTask::Pending {
  std::atomic<bool> done{};
  MeshCheckpointCompletion completion;
  PersistenceStamp stamp;
  ObjectRow target;
  std::shared_ptr<const MeshSet> prior, prepared;
  MeshCheckpointRequest names;
  std::vector<MeshReference> instances;
  std::size_t selected_instance{};
};
static_assert(std::is_nothrow_move_constructible_v<MeshCheckpointCompletion>);
MeshCheckpointTask::MeshCheckpointTask(RootFactory roots):roots_(std::move(roots)) {
  if(!roots_)roots_=[](std::string_view path){return platform::make_rooted_files(path);};
}
MeshCheckpointTask::~MeshCheckpointTask() {
  // Pending owns every worker input. Do not release it while a read is blocked.
  if(worker_.joinable())worker_.join();
}
Result<std::uint64_t> MeshCheckpointTask::start(const Document& document,
    std::shared_ptr<const MeshSet> prior,PersistentId selected,MeshCheckpointRequest names) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Mesh checkpoint admission requires its owner");
  if(busy())return fail(ErrorCode::Busy,"A mesh checkpoint request remains retained");
  if(document.playing())return fail(ErrorCode::Busy,"Stop Play before updating a mesh checkpoint");
  if(!prior||!prior->files()||!selected.valid()||!request_names(names))
    return fail(ErrorCode::InvalidInput,"Invalid selected mesh checkpoint request");
  if(names.recipe_root.capacity()>4096||names.checkpoint_root.capacity()>4096||
      names.project_leaf.capacity()>96||names.checkpoint_leaf.capacity()>96)
    return fail(ErrorCode::Exhausted,"Mesh checkpoint retained path capacity exceeded");
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Mesh checkpoint request IDs exhausted");
  try {
    const auto begin=Clock::now();
    auto pending=std::make_unique<Pending>();
    pending->stamp=document.persistence_stamp();
    auto rows=document.objects();if(!rows)return rows.error();
    if(rows.value().size()>render::viewport_max_draws)return fail(ErrorCode::Exhausted,"Mesh checkpoint instance capacity exceeded");
    auto target=std::find_if(rows.value().begin(),rows.value().end(),[&](const auto& row){return row.id==selected;});
    if(target==rows.value().end())return fail(ErrorCode::NotFound,"Selected checkpoint object is missing");
    if(!target->mesh||!target->mesh->valid())return fail(ErrorCode::InvalidInput,"Selected object must be a pinned mesh");
    auto retained=prior->find(*target->mesh);if(!retained)return retained.error();
    pending->target=*target;pending->prior=std::move(prior);pending->names=std::move(names);
    pending->instances.reserve(rows.value().size());
    for(const auto& row:rows.value())if(row.mesh) {
      if(row.id==selected)pending->selected_instance=pending->instances.size();
      pending->instances.push_back(*row.mesh);
    }
    auto admitted=pending->prior->validate_instances(pending->instances);if(!admitted)return admitted.error();
    auto& out=pending->completion;out.request=sequence_+1;out.selected=selected;
    out.previous=*target->mesh;out.replacement=out.previous;
    auto roots=roots_;out.snapshot_ms=elapsed(begin);
    auto running=std::thread([roots=std::move(roots),state=pending.get()]() mutable noexcept {
      // Declared first: all load/root locals unwind before publication. The
      // owner still joins physically before reading the terminal result.
      CompletionGuard complete{state->done};
      auto worker_roots=std::move(roots);
      auto& result=state->completion;
      bool preparing{};auto begin=Clock::now();
      try {
        auto root=worker_roots(state->names.recipe_root);
        if(!root){result.result=root.error();result.load_ms=elapsed(begin);return;}
        if(!root.value()){result.result=fail(ErrorCode::InvalidInput,"Mesh checkpoint recipe root is empty");result.load_ms=elapsed(begin);return;}
        auto project=content_project::load(*root.value(),state->names.project_leaf);
        if(!project){result.result=project.error();result.load_ms=elapsed(begin);return;}
        if(state->names.expected_project_revision&&project.value().revision()!=*state->names.expected_project_revision){
          result.result=fail(ErrorCode::InvalidDependency,"Displayed project revision changed before checkpoint update");
          result.load_ms=elapsed(begin);return;
        }
        auto checkpoints=worker_roots(state->names.checkpoint_root);
        if(!checkpoints){result.result=checkpoints.error();result.load_ms=elapsed(begin);return;}
        if(!checkpoints.value()){result.result=fail(ErrorCode::InvalidInput,"Mesh checkpoint root is empty");result.load_ms=elapsed(begin);return;}
        auto loaded=mesh_checkpoint::load(*checkpoints.value(),state->names.checkpoint_leaf,
            project.value(),*state->prior->files());
        result.load_ms=elapsed(begin);
        if(!loaded){result.result=loaded.error();return;}
        if(loaded.value().definition().id!=result.previous.asset) {
          result.result=fail(ErrorCode::InvalidDependency,"Checkpoint asset differs from the selected mesh");return;
        }
        result.replacement={loaded.value().definition().id,loaded.value().definition().cooked_revision};
        result.product_bytes=loaded.value().product().size();
        preparing=true;begin=Clock::now();
        if(result.previous==result.replacement)state->prepared=state->prior;
        else {
          auto prepared=MeshSet::extend(state->prior,loaded.value().product());
          if(!prepared){result.result=prepared.error();result.prepare_ms=elapsed(begin);return;}
          state->prepared=std::move(prepared).value();
        }
        // Replace the selected OBJECT'S slot only; another object may share the
        // old reference and must remain pinned to it.
        state->instances[state->selected_instance]=result.replacement;
        result.result=state->prepared->validate_instances(state->instances);
        if(result.result){
          // Material stays attached to this object through adoption. Admit its
          // candidate geometry binding before either Scene or cache publication.
          auto candidate=state->target;candidate.mesh=result.replacement;
          auto drawable=mesh_draw(candidate,state->prepared.get());
          if(!drawable)result.result=drawable.error();
        }
        result.prepare_ms=elapsed(begin);
      }catch(const std::bad_alloc&) {
        result.result=fail(ErrorCode::Exhausted,"Background mesh checkpoint allocation failed");
        (preparing?result.prepare_ms:result.load_ms)=elapsed(begin);
      }catch(...) {
        result.result=fail(ErrorCode::PlatformFailure,"Background mesh checkpoint preparation failed");
        (preparing?result.prepare_ms:result.load_ms)=elapsed(begin);
      }
    });
    // No throwing operation follows successful thread creation. Pending's
    // stable address and all captured inputs remain owned through join.
    pending_=std::move(pending);worker_=std::move(running);cancelled_=false;return ++sequence_;
  }catch(const std::bad_alloc&) {return fail(ErrorCode::Exhausted,"Mesh checkpoint admission allocation failed");}
   catch(...) {return fail(ErrorCode::PlatformFailure,"Mesh checkpoint worker could not start");}
}
Result<void> MeshCheckpointTask::cancel() {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Mesh checkpoint cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No mesh checkpoint request remains retained");
  cancelled_=true;return {};
}
Result<std::optional<MeshCheckpointCompletion>> MeshCheckpointTask::poll(Document& document,
    std::shared_ptr<const MeshSet>& current,PersistentId selection) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Mesh checkpoint completion requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))
    return std::optional<MeshCheckpointCompletion>{};
  try {worker_.join();}
  catch(...) {return fail(ErrorCode::PlatformFailure,"Mesh checkpoint worker join failed; ownership retained");}
  auto pending=std::move(pending_);auto& out=pending->completion;
  const auto begin=Clock::now();
  if(cancelled_)out.result=fail(ErrorCode::Cancelled,"Mesh checkpoint publication cancelled after worker quiescence");
  else if(out.result) {
    try {
      if(!document.current(pending->stamp)||document.playing()||current!=pending->prior||selection!=out.selected)
        out.result=fail(ErrorCode::StaleHandle,"Scene, assets or selected mesh changed while preparing");
      else {
        auto rows=document.objects();
        if(!rows)out.result=rows.error();
        else {
          auto row=std::find_if(rows.value().begin(),rows.value().end(),[&](const auto& item){return item.id==out.selected;});
          if(row==rows.value().end()||row->handle!=pending->target.handle||row->mesh!=pending->target.mesh)
            out.result=fail(ErrorCode::StaleHandle,"Selected mesh handle or reference changed while preparing");
          else if(!pending->prepared)out.result=fail(ErrorCode::Invariant,"Mesh checkpoint completed without prepared dependencies");
          else if(out.previous!=out.replacement) {
            row->mesh=out.replacement;
            out.result=document.edit(*row,pending->stamp.generation);
            if(out.result) {current.swap(pending->prepared);out.changed=true;}
          }
        }
      }
    }catch(const std::bad_alloc&) {out.result=fail(ErrorCode::Exhausted,"Mesh checkpoint owner publication allocation failed");}
     catch(...) {out.result=fail(ErrorCode::PlatformFailure,"Mesh checkpoint owner publication failed");}
  }
  out.commit_ms=elapsed(begin);
  return std::optional<MeshCheckpointCompletion>{std::move(out)};
}
} // namespace race::desktop
