#include "desktop_project_authoring.hpp"
#include <algorithm>
#include <array>
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
bool path(std::string_view value) noexcept {
  return !value.empty()&&value.size()<=4096&&value.find('\0')==std::string_view::npos;
}
bool leaf(std::string_view value,std::string_view extension) noexcept {
  return platform::valid_content_leaf(value)&&value.ends_with(extension);
}
Result<void> validate(const ProjectAuthoringRequest& names) noexcept {
  if(!names.epoch||!path(names.recipe_root)||!path(names.source_root)||!path(names.product_root)||
      !path(names.checkpoint_root)||!content_project::valid_new_index_leaf(names.new_index_leaf))
    return fail(ErrorCode::InvalidInput,"Invalid project authoring roots, index or epoch");
  if(names.recipe_root.capacity()>4096||names.source_root.capacity()>4096||
      names.product_root.capacity()>4096||names.checkpoint_root.capacity()>4096||
      names.new_index_leaf.capacity()>96||names.source_leaf.capacity()>96)
    return fail(ErrorCode::Exhausted,"Project authoring retained path capacity exceeded");
  if(names.profile!=mesh_pipeline::Profile::Geometry&&names.profile!=mesh_pipeline::Profile::GeometryUv)
    return fail(ErrorCode::InvalidInput,"Unsupported project recipe mesh profile");
  if(names.surface_profile){
    const auto info=surface_pipeline::profile_info(*names.surface_profile);
    if(!info)return info.error();
    if(names.profile!=mesh_pipeline::Profile::Geometry||
        (names.texture&&!names.texture->valid())||(!info.value().material&&names.texture)||
        (info.value().material&&info.value().mips&&!names.texture))
      return fail(ErrorCode::InvalidInput,"Project surface profile or texture key is incompatible");
  }else if(names.texture)return fail(ErrorCode::InvalidInput,"Mesh recipe cannot declare a texture key");
  if(names.kind==ProjectAuthoringKind::Create){
    if(names.asset.valid()||!names.source_leaf.empty()||names.profile!=mesh_pipeline::Profile::Geometry||names.surface_profile||names.texture)
      return fail(ErrorCode::InvalidInput,"Empty project creation has no recipe, asset or profile override");
  }else if(names.kind==ProjectAuthoringKind::Add){
    const auto extension=names.surface_profile?surface_pipeline::profile_info(*names.surface_profile).value().source_extension:std::string_view{".obj"};
    if(!names.asset.valid()||!leaf(names.source_leaf,extension))return fail(ErrorCode::InvalidInput,"Project recipe requires a stable asset and canonical profile source leaf");
  }else return fail(ErrorCode::InvalidInput,"Unsupported project authoring operation");
  return {};
}
Result<std::size_t> view_storage(const ProjectBrowserView& view) noexcept {
  if(!path(view.source.recipe_root)||!content_project::valid_index_leaf(view.source.project_leaf))
    return fail(ErrorCode::InvalidInput,"Invalid displayed project paths");
  if(view.source.recipe_root.capacity()>4096||view.source.project_leaf.capacity()>96)
    return fail(ErrorCode::Exhausted,"Displayed project retained path capacity exceeded");
  const auto retained=view.project.retained_bytes()+sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+
      view.source.recipe_root.capacity()+1+view.source.project_leaf.capacity()+1;
  if(retained>content_project::maximum_retained_bytes)return fail(ErrorCode::Exhausted,"Displayed project retained view capacity exceeded");
  return retained;
}
bool same_recipe(const content_project::Recipe& a,const content_project::Recipe& b) noexcept {
  return a.index()==b.index()&&std::visit([](const auto& left,const auto& right){
    return left.revision==right.revision&&left.bytes==right.bytes;
  },a,b);
}
Result<content_project::Recipe> load_recipe(platform::RootedFiles& files,std::string_view leaf,bool surface) {
  if(surface){auto loaded=surface_recipe::load(files,leaf);if(!loaded)return loaded.error();return content_project::Recipe{std::move(loaded).value()};}
  auto loaded=mesh_recipe::load(files,leaf);if(!loaded)return loaded.error();return content_project::Recipe{std::move(loaded).value()};
}
class BoundedRoot final:public platform::RootedFiles {
  std::shared_ptr<platform::RootedFiles> root_;
public:
  explicit BoundedRoot(std::shared_ptr<platform::RootedFiles> root) noexcept:root_(std::move(root)){}
  Result<std::vector<std::byte>> read(std::string_view name,std::size_t limit) override {
    auto value=root_->read(name,limit);if(!value)return value.error();
    if(value.value().size()>limit||value.value().capacity()>limit)
      return fail(ErrorCode::Exhausted,"Project authoring read capacity exceeded");return value;
  }
  Result<void> publish_immutable(std::string_view name,std::span<const std::byte> value) override {
    return root_->publish_immutable(name,value);
  }
  Result<void> replace(std::string_view name,std::span<const std::byte> value,platform::Durability durability) override {
    return root_->replace(name,value,durability);
  }
};
struct CompletionGuard {
  std::atomic<bool>& done;
  std::atomic<ProjectAuthoringPhase>& phase;
  ~CompletionGuard() noexcept {
    phase.store(ProjectAuthoringPhase::Complete,std::memory_order_release);
    done.store(true,std::memory_order_release);
  }
};
}
struct ProjectAuthoringTask::Pending {
  std::atomic<bool> done{},cancelled{};
  std::atomic<ProjectAuthoringPhase> phase{ProjectAuthoringPhase::Revalidating};
  ProjectAuthoringCompletion completion;
  std::shared_ptr<const ProjectBrowserView> prior,prepared;
};
static_assert(std::is_nothrow_move_constructible_v<ProjectAuthoringCompletion>);
ProjectAuthoringTask::ProjectAuthoringTask(RootFactory roots):roots_(std::move(roots)) {
  if(!roots_)roots_=[](std::string_view name){return platform::make_rooted_files(name);};
}
ProjectAuthoringTask::~ProjectAuthoringTask() {
  if(worker_.joinable()){
    pending_->cancelled.store(true,std::memory_order_release);worker_.join();
  }
}
Result<std::uint64_t> ProjectAuthoringTask::start(std::shared_ptr<const ProjectBrowserView> prior,
    ProjectAuthoringRequest names) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project authoring admission requires its owner");
  if(busy())return fail(ErrorCode::Busy,"A project authoring request remains retained");
  const auto begin=Clock::now();auto checked=validate(names);if(!checked)return checked.error();
  if(prior){auto retained=view_storage(*prior);if(!retained)return retained.error();}
  if(names.kind==ProjectAuthoringKind::Add){
    if(!prior)return fail(ErrorCode::InvalidInput,"Add recipe requires a displayed immutable project");
    if(names.recipe_root!=prior->source.recipe_root)
      return fail(ErrorCode::InvalidDependency,"Add recipe root differs from displayed project");
    if(names.new_index_leaf==prior->source.project_leaf)
      return fail(ErrorCode::Duplicate,"Add recipe requires a new project index name");
    if(prior->project.entries().size()==content_project::maximum_entries)
      return fail(ErrorCode::Exhausted,"Project recipe count limit");
    if(prior->project.find(names.asset))return fail(ErrorCode::Duplicate,"Project already contains the asset identity");
  }
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Project authoring request IDs exhausted");
  try {
    auto pending=std::make_unique<Pending>();pending->prior=std::move(prior);
    auto& out=pending->completion;out.request=sequence_+1;out.epoch=names.epoch;
    out.kind=names.kind;out.asset=names.asset;out.names=std::move(names);auto roots=roots_;
    auto running=std::thread([roots=std::move(roots),state=pending.get()]() mutable noexcept {
      // Publish readiness only after every local capability/byte owner and
      // moved callable has unwound. The UI must still physically join first.
      CompletionGuard complete{state->done,state->phase};auto worker_roots=std::move(roots);
      auto& out=state->completion;const auto& names=out.names;
      const auto cancelled=[&]() noexcept {
        if(!state->cancelled.load(std::memory_order_acquire))return false;
        out.result=fail(ErrorCode::Cancelled,"Project authoring cancelled; immutable evidence retained");return true;
      };
      const auto phase=[&](ProjectAuthoringPhase value) noexcept {state->phase.store(value,std::memory_order_release);};
      try {
        if(cancelled())return;const auto revalidate_begin=Clock::now();
        auto opened=worker_roots(names.recipe_root);if(!opened){out.result=opened.error();return;}
        if(!opened.value()){out.result=fail(ErrorCode::InvalidInput,"Project authoring recipe root is empty");return;}
        auto recipes=std::make_shared<BoundedRoot>(std::move(opened).value());
        // Existing destination always rejects, including identical bytes. Only
        // NotFound admits an immutable publication attempt later in this job.
        auto destination=recipes->read(names.new_index_leaf,content_project::maximum_bytes);
        if(destination){out.result=fail(ErrorCode::Duplicate,"New project index already exists");return;}
        if(destination.error().code!=ErrorCode::NotFound){out.result=destination.error();return;}
        if(cancelled())return;
        std::optional<content_project::Snapshot> original;
        if(names.kind==ProjectAuthoringKind::Add){
          auto loaded=content_project::load(*recipes,state->prior->source.project_leaf);
          if(!loaded){out.result=loaded.error();return;}
          if(loaded.value().revision()!=state->prior->project.revision()||
              !std::equal(loaded.value().bytes().begin(),loaded.value().bytes().end(),state->prior->project.bytes().begin(),state->prior->project.bytes().end())){
            out.result=fail(ErrorCode::InvalidDependency,"Displayed project raw index revision changed");return;
          }
          for(const auto& prior:state->prior->project.entries()){
            auto fresh=loaded.value().find(prior.reference.id);
            if(!fresh||!same_recipe(fresh.value()->recipe,prior.recipe)){
              out.result=fail(ErrorCode::InvalidDependency,"Displayed project raw recipe revision changed");return;
            }
          }
          original=std::move(loaded).value();
        }
        // Explicit caller roots are opened and retained, even though this task
        // does not import, cook, publish products or capture checkpoints.
        std::array<std::shared_ptr<platform::RootedFiles>,3> capabilities;
        const std::array<std::string_view,3> root_names{names.source_root,names.product_root,names.checkpoint_root};
        for(std::size_t n=0;n<root_names.size();++n){
          auto root=worker_roots(root_names[n]);if(!root){out.result=root.error();return;}
          if(!root.value()){out.result=fail(ErrorCode::InvalidInput,"Project authoring explicit root is empty");return;}
          capabilities[n]=std::move(root).value();if(cancelled())return;
        }
        out.revalidate_ms=elapsed(revalidate_begin);
        std::string recipe;assets::Digest recipe_revision{};
        if(names.kind==ProjectAuthoringKind::Add){
          phase(ProjectAuthoringPhase::ReadingSource);const auto source_begin=Clock::now();
          const auto source_limit=names.surface_profile?
              (surface_pipeline::profile_info(*names.surface_profile).value().material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes):mesh_pipeline::Limits{}.source_bytes;
          BoundedRoot source(capabilities[0]);auto read=source.read(names.source_leaf,source_limit);
          if(!read){out.result=read.error();return;}if(read.value().empty()){out.result=fail(ErrorCode::InvalidInput,"Project recipe source is empty");return;}
          out.source_bytes=read.value().size();out.source=assets::digest(read.value());out.source_ms=elapsed(source_begin);
          if(cancelled())return;
          auto encoded=names.surface_profile?
              surface_recipe::encode({names.asset,names.source_leaf,*names.surface_profile,names.texture}):
              mesh_recipe::encode({names.asset,names.source_leaf,names.profile});
          if(!encoded){out.result=encoded.error();return;}
          const auto recipe_limit=names.surface_profile?surface_recipe::maximum_bytes:mesh_recipe::maximum_bytes;
          if(encoded.value().size()>recipe_limit||encoded.value().capacity()>recipe_limit){
            out.result=fail(ErrorCode::Exhausted,"Authored recipe byte capacity exceeded");return;
          }
          recipe=std::move(encoded).value();recipe_revision=assets::digest(std::as_bytes(std::span(recipe)));
          const auto digest_leaf=assets::hex(recipe_revision)+".json";
          out.recipe_leaf=std::string(digest_leaf);
          if(out.recipe_leaf.capacity()>96){out.result=fail(ErrorCode::Exhausted,"Authored recipe leaf capacity exceeded");return;}
          if(out.recipe_leaf==names.new_index_leaf){out.result=fail(ErrorCode::Duplicate,"New index name collides with digest-named recipe");return;}
        }
        std::vector<content_project::Reference> references;
        const auto count=original?original->entries().size()+1:0;
        references.reserve(count);
        if(references.capacity()>content_project::maximum_entries){out.result=fail(ErrorCode::Exhausted,"Authored project reference capacity exceeded");return;}
        if(original)for(const auto& entry:original->entries())references.push_back(entry.reference);
        if(names.kind==ProjectAuthoringKind::Add){
          const auto kind=names.surface_profile?
              (surface_pipeline::profile_info(*names.surface_profile).value().material?content_project::Kind::Material:content_project::Kind::Texture):content_project::Kind::Mesh;
          references.push_back({names.asset,kind,out.recipe_leaf,recipe_revision});
        }
        // Every new snapshot uses the native binary index, including additions
        // from legacy JSON. Earlier index bytes and recipe pins remain immutable.
        auto encoded_index=content_project::encode_binary(references);
        if(!encoded_index){out.result=encoded_index.error();return;}
        auto index=std::move(encoded_index).value();
        if(index.capacity()>content_project::maximum_bytes){out.result=fail(ErrorCode::Exhausted,"Authored index byte capacity exceeded");return;}
        if(cancelled())return;
        phase(ProjectAuthoringPhase::Publishing);const auto publish_begin=Clock::now();
        if(names.kind==ProjectAuthoringKind::Add){
          // Existing digest leaf may be reused only through exact-byte immutable
          // publication, followed by exact readback before index publication.
          auto written=recipes->publish_immutable(out.recipe_leaf,std::as_bytes(std::span(recipe)));
          if(!written){out.result=written.error();return;}if(cancelled())return;
          auto readback=load_recipe(*recipes,out.recipe_leaf,bool(names.surface_profile));if(!readback){out.result=readback.error();return;}
          const auto expected=std::as_bytes(std::span(recipe));
          if(!std::visit([&](const auto& value){return value.revision==recipe_revision&&
              std::equal(value.bytes.begin(),value.bytes.end(),expected.begin(),expected.end());},readback.value())){
            out.result=fail(ErrorCode::IoFailure,"Authored recipe exact readback differs");return;
          }
          if(cancelled())return;
        }
        auto written=recipes->publish_immutable(names.new_index_leaf,std::as_bytes(std::span(index)));
        out.publish_ms=elapsed(publish_begin);if(!written){out.result=written.error();return;}if(cancelled())return;
        phase(ProjectAuthoringPhase::ReadingBack);const auto readback_begin=Clock::now();
        auto closure=content_project::load(*recipes,names.new_index_leaf);if(!closure){out.result=closure.error();return;}
        const auto expected=std::as_bytes(std::span(index));
        if(!std::equal(closure.value().bytes().begin(),closure.value().bytes().end(),expected.begin(),expected.end())){
          out.result=fail(ErrorCode::IoFailure,"Authored project exact readback differs");return;
        }
        if(cancelled())return;
        ProjectBrowserRequest browser{names.recipe_root,names.new_index_leaf,names.epoch};
        if(browser.recipe_root.capacity()>4096||browser.project_leaf.capacity()>96){out.result=fail(ErrorCode::Exhausted,"Authored project retained path capacity exceeded");return;}
        const auto retained=closure.value().retained_bytes()+sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+
            browser.recipe_root.capacity()+1+browser.project_leaf.capacity()+1;
        if(retained>content_project::maximum_retained_bytes){out.result=fail(ErrorCode::Exhausted,"Authored project retained view capacity exceeded");return;}
        state->prepared=std::make_shared<ProjectBrowserView>(std::move(browser),std::move(closure).value());
        out.entries=state->prepared->project.entries().size();out.retained_bytes=retained;out.readback_ms=elapsed(readback_begin);
      }catch(const std::bad_alloc&) {out.result=fail(ErrorCode::Exhausted,"Background project authoring allocation failed");}
       catch(...) {out.result=fail(ErrorCode::PlatformFailure,"Background project authoring failed");}
    });
    pending_=std::move(pending);worker_=std::move(running);
    pending_->completion.admission_ms=elapsed(begin);return ++sequence_;
  }catch(const std::bad_alloc&) {return fail(ErrorCode::Exhausted,"Project authoring admission allocation failed");}
   catch(...) {return fail(ErrorCode::PlatformFailure,"Project authoring worker could not start");}
}
Result<void> ProjectAuthoringTask::cancel() {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project authoring cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No project authoring request remains retained");
  pending_->cancelled.store(true,std::memory_order_release);return {};
}
Result<ProjectAuthoringProgress> ProjectAuthoringTask::progress() const noexcept {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project authoring progress requires its owner");
  if(!busy())return ProjectAuthoringProgress{};
  return ProjectAuthoringProgress{pending_->phase.load(std::memory_order_acquire),pending_->cancelled.load(std::memory_order_acquire)};
}
Result<std::optional<ProjectAuthoringCompletion>> ProjectAuthoringTask::poll(std::uint64_t epoch,
    std::shared_ptr<const ProjectBrowserView>& visible) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project authoring completion requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))return std::optional<ProjectAuthoringCompletion>{};
  try {worker_.join();}catch(...) {return fail(ErrorCode::PlatformFailure,"Project authoring worker join failed; ownership retained");}
  auto pending=std::move(pending_);auto& out=pending->completion;const auto begin=Clock::now();
  if(pending->cancelled.load(std::memory_order_acquire))out.result=fail(ErrorCode::Cancelled,"Project authoring publication cancelled after worker quiescence");
  else if(epoch!=out.epoch||visible!=pending->prior)out.result=fail(ErrorCode::StaleHandle,"Displayed project or epoch changed while authoring");
  else if(out.result){
    if(!pending->prepared)out.result=fail(ErrorCode::Invariant,"Project authoring completed without verified closure");
    else {visible.swap(pending->prepared);out.changed=true;}
  }
  out.publication_ms=elapsed(begin);return std::optional<ProjectAuthoringCompletion>{std::move(out)};
}
} // namespace race::desktop
