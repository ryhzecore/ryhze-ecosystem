#include "desktop_project_browser.hpp"
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
bool request_names(const ProjectBrowserRequest& names) noexcept {
  return names.epoch&& !names.recipe_root.empty()&&names.recipe_root.size()<=4096&&
      names.recipe_root.find('\0')==std::string::npos&&
      content_project::valid_index_leaf(names.project_leaf);
}
struct CompletionGuard {
  std::atomic<bool>& done;
  ~CompletionGuard() noexcept{done.store(true,std::memory_order_release);}
};
}
struct ProjectBrowserTask::Pending {
  std::atomic<bool> done{};
  ProjectBrowserCompletion completion;
  ProjectBrowserRequest names;
  std::shared_ptr<const ProjectBrowserView> prepared;
};
static_assert(std::is_nothrow_move_constructible_v<ProjectBrowserCompletion>);
ProjectBrowserTask::ProjectBrowserTask(RootFactory roots):roots_(std::move(roots)) {
  if(!roots_)roots_=[](std::string_view path){return platform::make_rooted_files(path);};
}
ProjectBrowserTask::~ProjectBrowserTask() {
  // A timeout or cancellation cannot release paths/root owners during a read.
  if(worker_.joinable())worker_.join();
}
Result<std::uint64_t> ProjectBrowserTask::start(ProjectBrowserRequest names) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project browser admission requires its owner");
  if(busy())return fail(ErrorCode::Busy,"A project browser request remains retained");
  const auto begin=Clock::now();
  if(!request_names(names))return fail(ErrorCode::InvalidInput,"Invalid project browser request");
  if(names.recipe_root.capacity()>4096||names.project_leaf.capacity()>96)
    return fail(ErrorCode::Exhausted,"Project browser retained path capacity exceeded");
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Project browser request IDs exhausted");
  try {
    auto pending=std::make_unique<Pending>();pending->names=std::move(names);
    auto& out=pending->completion;out.request=sequence_+1;out.epoch=pending->names.epoch;
    auto roots=roots_;
    auto running=std::thread([roots=std::move(roots),state=pending.get()]() mutable noexcept {
      // The first local publishes readiness last, after root/load locals and
      // the moved callable have unwound. poll still joins physically first.
      CompletionGuard complete{state->done};auto worker_roots=std::move(roots);
      auto& result=state->completion;const auto begin=Clock::now();
      try {
        auto root=worker_roots(state->names.recipe_root);
        if(!root){result.result=root.error();result.read_ms=elapsed(begin);return;}
        if(!root.value()){result.result=fail(ErrorCode::InvalidInput,"Project browser root is empty");result.read_ms=elapsed(begin);return;}
        auto project=content_project::load(*root.value(),state->names.project_leaf);
        if(!project){result.result=project.error();result.read_ms=elapsed(begin);return;}
        const auto retained=project.value().retained_bytes();
        const auto extra=sizeof(ProjectBrowserView)-sizeof(content_project::Snapshot)+
            state->names.recipe_root.capacity()+1+state->names.project_leaf.capacity()+1;
        if(retained>content_project::maximum_retained_bytes||extra>content_project::maximum_retained_bytes-retained){
          result.result=fail(ErrorCode::Exhausted,"Project browser retained view capacity exceeded");result.read_ms=elapsed(begin);return;
        }
        // Every row, raw byte and retained name is prepared before publication.
        // No source/product discovery or mutable latest-revision inference.
        state->prepared=std::make_shared<ProjectBrowserView>(std::move(state->names),std::move(project).value());
        result.entries=state->prepared->project.entries().size();
        result.retained_bytes=retained+extra;
        result.read_ms=elapsed(begin);
      }catch(const std::bad_alloc&) {
        result.result=fail(ErrorCode::Exhausted,"Background project browser allocation failed");result.read_ms=elapsed(begin);
      }catch(...) {
        result.result=fail(ErrorCode::PlatformFailure,"Background project browser read failed");result.read_ms=elapsed(begin);
      }
    });
    // Only no-throw operations follow successful creation. The pending address
    // stays stable and all worker captures remain owned until physical join.
    pending_=std::move(pending);worker_=std::move(running);cancelled_=false;
    pending_->completion.admission_ms=elapsed(begin);return ++sequence_;
  }catch(const std::bad_alloc&) {return fail(ErrorCode::Exhausted,"Project browser admission allocation failed");}
   catch(...) {return fail(ErrorCode::PlatformFailure,"Project browser worker could not start");}
}
Result<void> ProjectBrowserTask::cancel() {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project browser cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No project browser request remains retained");
  cancelled_=true;return {};
}
Result<std::optional<ProjectBrowserCompletion>> ProjectBrowserTask::poll(
    std::uint64_t current_epoch,std::shared_ptr<const ProjectBrowserView>& visible) {
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Project browser completion requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))return std::optional<ProjectBrowserCompletion>{};
  try {worker_.join();}
  catch(...) {return fail(ErrorCode::PlatformFailure,"Project browser worker join failed; ownership retained");}
  auto pending=std::move(pending_);auto& out=pending->completion;const auto begin=Clock::now();
  if(cancelled_)out.result=fail(ErrorCode::Cancelled,"Project browser publication cancelled after worker quiescence");
  else if(current_epoch!=out.epoch)out.result=fail(ErrorCode::StaleHandle,"Project browser epoch changed while reading");
  else if(out.result) {
    if(!pending->prepared)out.result=fail(ErrorCode::Invariant,"Project browser completed without a validated snapshot");
    else {visible.swap(pending->prepared);out.changed=true;}
  }
  out.publication_ms=elapsed(begin);return std::optional<ProjectBrowserCompletion>{std::move(out)};
}
} // namespace race::desktop
