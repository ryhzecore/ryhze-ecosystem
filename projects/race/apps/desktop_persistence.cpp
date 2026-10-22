#include "desktop_persistence.hpp"
#include <atomic>
#include <chrono>
#include <limits>
#include <system_error>
#include <type_traits>

namespace race::desktop {
static_assert(std::is_nothrow_move_constructible_v<PersistenceCompletion>);
struct PersistenceTask::Pending {
  PersistenceCompletion completion;
  std::atomic<bool> done{};
  std::shared_ptr<const scene::Schemas> schema;
  std::optional<scene::Document> snapshot;
  Prepare prepare;
  Decode decode;
};
PersistenceTask::PersistenceTask(std::unique_ptr<platform::Files> files):files_(std::move(files)){}
PersistenceTask::~PersistenceTask(){ if(worker_.joinable())worker_.join(); }
Result<std::uint64_t> PersistenceTask::start(const Document& document,PersistenceKind kind,std::string path,Prepare prepare,Decode decode){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Persistence admission requires its owner");
  if(busy())return fail(ErrorCode::Busy,"A Scene file operation is still retained");
  if(!files_||path.empty()||path.size()>4096||path.find('\0')!=std::string::npos)
    return fail(ErrorCode::InvalidInput,"Invalid Scene file request");
  if(kind!=PersistenceKind::Save&&kind!=PersistenceKind::Cook&&kind!=PersistenceKind::Checkpoint&&!reads_scene(kind))
    return fail(ErrorCode::InvalidInput,"Unknown Scene file operation");
  if((kind==PersistenceKind::PackageLoad)!=bool(decode))return fail(ErrorCode::InvalidInput,"Package loads require an explicit composed decoder");
  if(reads_scene(kind)&&document.playing())return fail(ErrorCode::Busy,"Stop Play before loading or recovering");
  if(sequence_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Scene file request IDs exhausted");
  try {
    const auto snapshot_begin=std::chrono::steady_clock::now();
    std::optional<scene::Document> snapshot;
    if(!reads_scene(kind)){
      auto copy=document.snapshot();if(!copy)return copy.error();snapshot=std::move(copy).value();
    }
    auto state=std::make_unique<Pending>();auto& completion=state->completion;
    completion.snapshot_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-snapshot_begin).count();
    completion.request=sequence_+1;completion.kind=kind;
    completion.path=std::move(path);completion.stamp=document.persistence_stamp();
    state->schema=document.schemas();state->snapshot=std::move(snapshot);
    state->prepare=std::move(prepare);state->decode=std::move(decode);
    std::thread worker([files=files_.get(),state=state.get()]() noexcept {
      struct CompletionGuard {
        Pending& state;
        ~CompletionGuard(){
          // Callable captures may retain arbitrary owned dependencies. Finish
          // their cleanup before poll observes readiness; join then confirms
          // that the physical thread has returned.
          state.prepare={};state.decode={};state.snapshot.reset();state.schema.reset();
          state.done.store(true,std::memory_order_release);
        }
      } completed{*state};
      auto* out=&state->completion;
      const auto begin=std::chrono::steady_clock::now();
      try {
        if(reads_scene(out->kind)){
          auto decoded=state->decode?state->decode(*files,out->path,*state->schema):Document::decode(*files,out->path,*state->schema);
          if(decoded){
            if(state->prepare)out->result=state->prepare(decoded.value());
            if(out->result)out->loaded=std::move(decoded).value();
          }else out->result=decoded.error();
        } else out->result=Document::write(*files,out->path,*state->snapshot,*state->schema,out->kind==PersistenceKind::Cook);
      } catch(const std::bad_alloc&) {out->result=fail(ErrorCode::Exhausted,"Background Scene file allocation failed");}
        catch(...) {out->result=fail(ErrorCode::PlatformFailure,"Background Scene file operation failed");}
      out->worker_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    });
    pending_=std::move(state);worker_=std::move(worker);kind_=kind;cancelled_=false;return ++sequence_;
  } catch(const std::bad_alloc&) {return fail(ErrorCode::Exhausted,"Scene file admission allocation failed");}
    catch(const std::system_error&) {return fail(ErrorCode::PlatformFailure,"Scene file worker could not start");}
    catch(...) {return fail(ErrorCode::PlatformFailure,"Scene file admission failed");}
}
Result<void> PersistenceTask::cancel_load(){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Persistence cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No Scene file operation is retained");
  if(!reads_scene(kind_))return fail(ErrorCode::Unsupported,"An admitted write must finish its atomic replacement");
  cancelled_=true;return {};
}
Result<std::optional<PersistenceCompletion>> PersistenceTask::poll(Document& document){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Persistence completion requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))
    return std::optional<PersistenceCompletion>{};
  worker_.join();
  auto result=std::move(pending_->completion);pending_.reset();
  result.matches_current=document.current(result.stamp);
  if(reads_scene(result.kind)){
    if(cancelled_)result.result=fail(ErrorCode::Cancelled,"Scene load cancelled after worker quiescence");
    else if(result.result){
      if(!result.loaded)result.result=fail(ErrorCode::Invariant,"Scene load completed without a document");
      else try {result.result=document.install(*result.loaded,result.stamp);}
      catch(const std::bad_alloc&) {result.result=fail(ErrorCode::Exhausted,"Scene publication allocation failed");}
      catch(...) {result.result=fail(ErrorCode::PlatformFailure,"Scene publication failed");}
    }
    result.loaded.reset();
  }
  return std::optional<PersistenceCompletion>{std::move(result)};
}
}
