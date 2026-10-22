#include "../apps/desktop_persistence.hpp"
#include "../apps/desktop_profile.hpp"
#include "test.hpp"
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>

using namespace race;
using namespace race::desktop;
namespace {
struct State {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered{},release{},fail{},destroyed{},operation_finished{};
  int throws{};
  std::vector<std::byte> bytes;
};
struct GatedFiles final : platform::Files {
  std::shared_ptr<State> state;
  explicit GatedFiles(std::shared_ptr<State> value):state(std::move(value)){}
  ~GatedFiles(){std::lock_guard lock(state->mutex);CHECK(!state->entered||state->operation_finished);state->destroyed=true;state->cv.notify_all();}
  void gate(){
    std::unique_lock lock(state->mutex);state->entered=true;state->operation_finished=false;state->cv.notify_all();
    state->cv.wait(lock,[&]{return state->release;});
  }
  struct Finished {
    std::shared_ptr<State> state;
    ~Finished(){std::lock_guard lock(state->mutex);state->operation_finished=true;state->cv.notify_all();}
  };
  void fault(){if(state->throws==1)throw std::bad_alloc{};if(state->throws==2)throw std::runtime_error("injected exception");}
  Result<std::vector<std::byte>> read(std::string_view,std::size_t limit) override {
    Finished finished{state};gate();fault();if(state->fail)return fail(ErrorCode::IoFailure,"injected read failure");
    if(state->bytes.size()>limit)return fail(ErrorCode::Exhausted,"read budget");return state->bytes;
  }
  Result<void> replace(std::string_view,std::span<const std::byte> bytes,platform::Durability durability) override {
    Finished finished{state};CHECK(durability==platform::Durability::AtomicVisibility);gate();fault();
    if(state->fail)return fail(ErrorCode::IoFailure,"injected write failure");
    state->bytes.assign(bytes.begin(),bytes.end());return {};
  }
};
std::unique_ptr<Document> document(){auto d=Document::create();CHECK(d);return std::move(d).value();}
void entered(const std::shared_ptr<State>& state){
  std::unique_lock lock(state->mutex);
  CHECK(state->cv.wait_for(lock,std::chrono::seconds{5},[&]{return state->entered;}));
}
void release(const std::shared_ptr<State>& state){
  std::lock_guard lock(state->mutex);state->release=true;state->cv.notify_all();
}
PersistenceCompletion completion(PersistenceTask& task,Document& d,std::size_t allocation_step=SIZE_MAX){
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
  for(;;){
    auto polled=task.poll(d);CHECK(polled);if(polled.value())return std::move(*polled.value());
    if(std::chrono::steady_clock::now()>=deadline){
      std::fprintf(stderr,"Persistence completion timed out; admission allocation step=%zu\n",allocation_step);CHECK(false);
    }
    std::this_thread::yield();
  }
}
std::shared_ptr<State> source(Document& d){
  auto state=std::make_shared<State>();state->release=true;
  {GatedFiles files(state);CHECK(d.save(files,"source"));}
  // The completed fixture writer is distinct from the retained task's Files.
  state->entered=false;state->release=false;state->operation_finished=false;state->destroyed=false;return state;
}
void edit(Document& d){auto rows=d.objects();CHECK(rows);auto row=rows.value().front();row.local.translation.x+=3;CHECK(d.edit(row,d.generation()));}
void delayed_save_and_capacity(){
  auto d=document();const auto original=d->snapshot();CHECK(original);
  auto state=std::make_shared<State>();PersistenceTask task(std::make_unique<GatedFiles>(state));
  auto request=task.start(*d,PersistenceKind::Save,"save");CHECK(request);entered(state);
  CHECK(task.busy());auto pending=task.poll(*d);CHECK(pending&&!pending.value());
  CHECK_ERROR(task.start(*d,PersistenceKind::Cook,"other"),ErrorCode::Busy);
  CHECK_ERROR(task.cancel_load(),ErrorCode::Unsupported);
  edit(*d);release(state);auto result=completion(task,*d);
  CHECK(result.request==request.value());CHECK(result.result);CHECK(!result.matches_current);CHECK(!task.busy());
  auto decoded=scene::decode_json({reinterpret_cast<const char*>(state->bytes.data()),state->bytes.size()},*d->schemas(),{});CHECK(decoded);
  auto expected=scene::encode_json(original.value(),*d->schemas(),{});CHECK(expected);
  CHECK(std::string(reinterpret_cast<const char*>(state->bytes.data()),state->bytes.size())==expected.value());
  CHECK(d->objects().value().front().local.translation.x==3);
  CHECK(d->undo());CHECK(d->objects().value().front().local.translation.x==0);
  CHECK(!task.poll(*d).value());
}
void stale_loads_and_cancellation(){
  for(auto kind:{PersistenceKind::Load,PersistenceKind::Recover})for(int variant=0;variant<4;++variant){
    auto d=document();auto state=source(*d);PersistenceTask task(std::make_unique<GatedFiles>(state));
    CHECK(task.start(*d,kind,"load"));entered(state);
    auto other=document();
    if(variant==0)edit(*d);
    if(variant==1){CHECK(d->start_play());CHECK(d->stop_play());}
    if(variant==2)CHECK(task.cancel_load());
    auto& destination=variant==3?*other:*d;const auto generation=destination.generation();
    release(state);auto result=completion(task,destination);
    CHECK_ERROR(result.result,variant==2?ErrorCode::Cancelled:ErrorCode::StaleHandle);
    CHECK(destination.generation()==generation);CHECK(!result.loaded);
    if(variant==0){CHECK(d->undo());CHECK(d->objects().value().front().local.translation.x==0);}
  }
}
void success_failure_and_owner(){
  auto d=document();auto state=source(*d);const auto original=state->bytes;
  PersistenceTask task(std::make_unique<GatedFiles>(state));
  edit(*d);CHECK(task.start(*d,PersistenceKind::Load,"load"));entered(state);release(state);
  CHECK(completion(task,*d).result);CHECK(d->objects().value().front().local.translation.x==0);
  CHECK(!d->undo());CHECK(!d->redo());
  CHECK(task.start(*d,PersistenceKind::Cook,"cooked"));CHECK(completion(task,*d).result);
  CHECK(state->bytes.size()>8&&std::memcmp(state->bytes.data(),"RACESCN1",8)==0);
  CHECK(task.start(*d,PersistenceKind::Load,"cooked"));CHECK(completion(task,*d).result);
  CHECK(task.start(*d,PersistenceKind::Save,"json"));CHECK(completion(task,*d).result);CHECK(state->bytes==original);
  state->fail=true;const auto generation=d->generation();
  CHECK(task.start(*d,PersistenceKind::Save,"json"));CHECK_ERROR(completion(task,*d).result,ErrorCode::IoFailure);CHECK(state->bytes==original);
  CHECK(task.start(*d,PersistenceKind::Load,"json"));CHECK_ERROR(completion(task,*d).result,ErrorCode::IoFailure);CHECK(d->generation()==generation);
  state->fail=false;state->bytes.resize(12);
  CHECK(task.start(*d,PersistenceKind::Load,"bad"));CHECK(!completion(task,*d).result);CHECK(d->generation()==generation);
  std::thread wrong([&]{
    CHECK_ERROR(task.start(*d,PersistenceKind::Save,"x"),ErrorCode::WrongThread);
    CHECK_ERROR(task.cancel_load(),ErrorCode::WrongThread);CHECK_ERROR(task.poll(*d),ErrorCode::WrongThread);
  });wrong.join();
  CHECK_ERROR(task.start(*d,PersistenceKind::Save,""),ErrorCode::InvalidInput);
  CHECK_ERROR(task.start(*d,PersistenceKind::Save,std::string(4097,'a')),ErrorCode::InvalidInput);
  CHECK_ERROR(task.start(*d,PersistenceKind::Save,std::string("a\0b",3)),ErrorCode::InvalidInput);
  CHECK(d->start_play());CHECK_ERROR(task.start(*d,PersistenceKind::Load,"x"),ErrorCode::Busy);CHECK(d->stop_play());
}
void destruction_retains_io(){
  for(auto kind:{PersistenceKind::Save,PersistenceKind::Checkpoint,PersistenceKind::Recover}){
  auto d=document();auto state=kind==PersistenceKind::Recover?source(*d):std::make_shared<State>();
  auto task=std::make_unique<PersistenceTask>(std::make_unique<GatedFiles>(state));
  CHECK(task->start(*d,kind,"retained-io"));entered(state);
  // Document can die: workers hold owned snapshots/schema/identity only.
  d.reset();
  std::promise<void> shutdown_requested;
  auto requested=shutdown_requested.get_future();
  std::thread destroy([owned=std::move(task),&shutdown_requested]() mutable {
    shutdown_requested.set_value();owned.reset();
  });
  requested.get();
  {std::lock_guard lock(state->mutex);CHECK(!state->destroyed);CHECK(!state->operation_finished);}
  release(state);destroy.join();CHECK(state->operation_finished&&state->destroyed);CHECK(!state->bytes.empty());
  }
}
void throwing_io_has_terminal_completion(){
  for(auto kind:{PersistenceKind::Save,PersistenceKind::Cook,PersistenceKind::Load,PersistenceKind::Checkpoint,PersistenceKind::Recover})for(int fault=1;fault<=2;++fault){
    auto d=document();auto state=source(*d);state->throws=fault;
    PersistenceTask task(std::make_unique<GatedFiles>(state));
    const auto generation=d->generation();const auto original=state->bytes;
    auto admitted=task.start(*d,kind,"throwing-operation");CHECK(admitted);entered(state);release(state);
    auto result=completion(task,*d);
    CHECK(result.request==admitted.value()&&result.kind==kind&&result.path=="throwing-operation");
    CHECK_ERROR(result.result,fault==1?ErrorCode::Exhausted:ErrorCode::PlatformFailure);
    CHECK(!task.busy());CHECK(!task.poll(*d).value());CHECK(d->generation()==generation);CHECK(state->bytes==original);
    state->throws=0;CHECK(task.start(*d,PersistenceKind::Save,"retry"));CHECK(completion(task,*d).result);
  }
}
void allocation_admission_has_terminal_completion(){
  // MSVC's std::async provider could accept admission after a failed allocation
  // yet leave its future permanently nonready. Sweep the real owner allocation
  // path: every request must reject synchronously or deliver one completion.
  for(auto kind:{PersistenceKind::Save,PersistenceKind::Load}){
    std::size_t rejected{},completed{};
    for(std::size_t step=0;step<96;++step){
      auto d=document();edit(*d);auto state=source(*d);state->release=true;
      const auto generation=d->generation();const auto original=state->bytes;
      PersistenceTask task(std::make_unique<GatedFiles>(state));std::string path="allocation-admission";
      profile::testing::fail_allocation_after(step);
      auto admitted=task.start(*d,kind,std::move(path));
      profile::testing::fail_next_allocation_attempts(0);
      if(!admitted){
        ++rejected;CHECK_ERROR(admitted,ErrorCode::Exhausted);
        CHECK(!task.busy()&&!state->entered&&state->bytes==original&&d->generation()==generation);
        // A rejected start consumes neither capacity nor a request ID.
        admitted=task.start(*d,kind,"allocation-admission");CHECK(admitted);
      }else ++completed;
      CHECK(admitted.value()==1&&task.busy());
      auto result=completion(task,*d,step);
      CHECK(result.result&&result.request==1&&result.kind==kind&&result.path=="allocation-admission");
      CHECK(result.matches_current&&result.snapshot_ms>=0&&result.worker_ms>=0);
      CHECK(!task.busy()&&!task.poll(*d).value()&&state->operation_finished&&state->bytes==original);
      CHECK(d->objects().value().front().local.translation.x==3);
      auto retry=task.start(*d,PersistenceKind::Save,"after-completion");CHECK(retry&&retry.value()==2);
      CHECK(completion(task,*d,step).result&&!task.busy());
    }
    CHECK(rejected>0&&completed>0);
  }
}
void checkpoints_and_recovery(){
  auto d=document();auto state=std::make_shared<State>();PersistenceTask task(std::make_unique<GatedFiles>(state));
  edit(*d);const auto generation=d->generation();const auto snapshot=d->snapshot();CHECK(snapshot);
  auto encoded=scene::encode_json(snapshot.value(),*d->schemas(),{});CHECK(encoded);
  auto request=task.start(*d,PersistenceKind::Checkpoint,"recovery.scene.json");CHECK(request);entered(state);
  CHECK(d->generation()==generation);CHECK_ERROR(task.start(*d,PersistenceKind::Recover,"recovery.scene.json"),ErrorCode::Busy);
  CHECK_ERROR(task.cancel_load(),ErrorCode::Unsupported);edit(*d);release(state);
  auto result=completion(task,*d);CHECK(result.result&&result.kind==PersistenceKind::Checkpoint&&!result.matches_current);
  CHECK(std::string(reinterpret_cast<const char*>(state->bytes.data()),state->bytes.size())==encoded.value());
  CHECK(d->objects().value().front().local.translation.x==6);CHECK(d->undo());CHECK(d->objects().value().front().local.translation.x==3);
  CHECK(d->redo());CHECK(d->objects().value().front().local.translation.x==6);
  const auto saved_bytes=state->bytes;const auto edited_generation=d->generation();state->bytes.resize(12);
  CHECK(task.start(*d,PersistenceKind::Recover,"malformed-with-history"));CHECK(!completion(task,*d).result);
  CHECK(d->generation()==edited_generation);CHECK(d->undo());CHECK(d->objects().value().front().local.translation.x==3);
  CHECK(d->redo());CHECK(d->objects().value().front().local.translation.x==6);state->bytes=saved_bytes;
  CHECK(task.start(*d,PersistenceKind::Recover,"recovery.scene.json"));CHECK(completion(task,*d).result);
  CHECK(d->objects().value().front().local.translation.x==3);CHECK(!d->undo());CHECK(!d->redo());
  const auto restored=d->generation();const auto original=state->bytes;state->bytes.resize(12);
  CHECK(task.start(*d,PersistenceKind::Recover,"malformed"));CHECK(!completion(task,*d).result);CHECK(d->generation()==restored);
  state->bytes=original;state->fail=true;
  CHECK(task.start(*d,PersistenceKind::Checkpoint,"failed-checkpoint"));CHECK_ERROR(completion(task,*d).result,ErrorCode::IoFailure);
  CHECK(state->bytes==original&&d->generation()==restored);state->fail=false;
  CHECK(d->start_play());CHECK(d->tick_play(.1));
  CHECK_ERROR(task.start(*d,PersistenceKind::Recover,"during-play"),ErrorCode::Busy);
  CHECK(task.start(*d,PersistenceKind::Checkpoint,"during-play"));CHECK(completion(task,*d).result);CHECK(state->bytes==original);
  CHECK(d->stop_play());CHECK(d->generation()==restored);
  CHECK_ERROR(task.start(*d,static_cast<PersistenceKind>(255),"unknown"),ErrorCode::InvalidInput);
}
}
int main(){
  delayed_save_and_capacity();stale_loads_and_cancellation();success_failure_and_owner();destruction_retains_io();throwing_io_has_terminal_completion();allocation_admission_has_terminal_completion();checkpoints_and_recovery();
  std::puts("desktop persistence: owned snapshots, bounded admission, stale/cancelled loads, atomic failures and quiescence passed");
}
