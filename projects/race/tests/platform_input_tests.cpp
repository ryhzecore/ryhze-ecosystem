#include "race/platform/input.hpp"
#include "../apps/desktop_navigation.hpp"
#include "../apps/desktop_profile.hpp"
#include "test.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
using namespace race;
using namespace race::platform;
using namespace race::platform::input;
namespace {
struct FakeClock final : Clock {
  MonotonicTime time{100,7};
  MonotonicTime now() const noexcept override{return time;}
};
struct FakeSource final : Source {
  std::array<Registration,17> registrations{};
  std::size_t registered{},size{},cursor{},calls{};
  std::array<SourceEvent,2048> records{};
  std::optional<std::size_t> fail_at;
  bool* destroyed{};
  FakeClock* change_clock{};MonotonicTime change_to{};
  ~FakeSource() override{if(destroyed)*destroyed=true;}
  std::span<const Registration> initial_devices() const noexcept override{return {registrations.data(),registered};}
  Result<std::optional<SourceEvent>> next() noexcept override{
    ++calls;
    if(change_clock&&cursor==1)change_clock->time=change_to;
    if(fail_at&&cursor==*fail_at)return fail(ErrorCode::IoFailure,"Injected source failure");
    if(cursor==size)return std::optional<SourceEvent>{};
    return std::optional<SourceEvent>{records[cursor++]};
  }
  void push(SourceEvent record){CHECK(size<records.size());records[size++]=record;}
};
SourceEvent key(std::uint64_t instance=0){
  SourceEvent out;out.kind=Kind::Key;out.key=Key::W;out.pressed=true;out.instance=instance;
  out.provenance=instance?Provenance::Registered:Provenance::Unknown;return out;
}
SourceEvent lifecycle(Kind kind){SourceEvent out;out.kind=kind;return out;}
SourceEvent device(Kind kind,std::uint64_t instance){
  auto out=key(instance);out.kind=kind;out.key=Key::None;out.pressed=false;return out;
}
struct Fixture {
  FakeClock clock;
  std::unique_ptr<FakeSource> pending=std::make_unique<FakeSource>();
  FakeSource* source=pending.get();
  std::unique_ptr<Events> events;
  std::unique_ptr<std::array<Event,batch_capacity>> storage=std::make_unique<std::array<Event,batch_capacity>>();
  std::span<Event> output{*storage};
  void start(Limits limits={}){
    auto result=make_events(std::move(pending),clock,11,limits);CHECK(result);events=std::move(result).value();
  }
  Batch poll(std::size_t count=batch_capacity,std::size_t work=work_capacity){
    auto result=events->poll({output.data(),count},work);CHECK(result);return result.value();
  }
};
void bounds(){
  Fixture f;for(std::size_t i=0;i<129;++i)f.source->push(key());f.start();
  CHECK_ERROR(f.events->poll({}),ErrorCode::InvalidInput);
  CHECK_ERROR(f.events->poll(f.output,0),ErrorCode::InvalidInput);
  CHECK_ERROR(f.events->poll(f.output,work_capacity+1),ErrorCode::InvalidInput);
  auto too_many=std::make_unique<std::array<Event,129>>();CHECK_ERROR(f.events->poll(*too_many),ErrorCode::InvalidInput);
  CHECK(f.source->calls==0);
  auto batch=f.poll();CHECK(batch.written==128&&batch.examined==128&&batch.output_full&&!batch.observed_empty);
  CHECK(f.source->cursor==128&&f.source->calls==128);CHECK(f.output[127].sequence==128);
  batch=f.poll();CHECK(batch.written==1&&batch.observed_empty&&f.output[0].sequence==129);
  Fixture ignored;for(std::size_t i=0;i<work_capacity;++i)ignored.source->push({});ignored.source->push(key());ignored.start();
  batch=ignored.poll();CHECK(batch.examined==1024&&batch.ignored==1024&&batch.written==0&&batch.work_limit_reached);
  CHECK(ignored.source->cursor==1024);batch=ignored.poll();CHECK(batch.written==1&&batch.observed_empty);
  bool destroyed=false;Fixture owned;owned.source->destroyed=&destroyed;owned.source->push(key());owned.start();
  CHECK(owned.events->close());CHECK(owned.events->close());CHECK(!destroyed);
  CHECK_ERROR(owned.events->poll(owned.output),ErrorCode::Stopping);CHECK(owned.source->calls==0);
  owned.events.reset();CHECK(destroyed);
}
void identities(){
  Fixture f;f.source->registrations[0]={DeviceKind::Keyboard,1};f.source->registered=1;
  f.source->push(key(1));f.source->push(device(Kind::DeviceAdded,1));
  f.source->push(device(Kind::DeviceRemoved,1));f.source->push(key(1));
  f.source->push(device(Kind::DeviceAdded,2));f.source->push(key(2));f.start();
  auto batch=f.poll();CHECK(batch.written==5&&batch.examined==6&&batch.stale_devices==1);
  CHECK(f.output[0].device.generation==1&&f.output[0].epoch==1);
  CHECK(f.output[1].device==f.output[0].device);CHECK(f.output[2].epoch==2);
  CHECK(f.output[4].device.slot==f.output[0].device.slot&&f.output[4].device.generation==2);
  CHECK(f.events->status().value().devices==1);
  Fixture retired;retired.source->push(device(Kind::DeviceAdded,1));retired.source->push(device(Kind::DeviceRemoved,1));
  retired.source->push(device(Kind::DeviceAdded,2));retired.source->push(device(Kind::DeviceRemoved,2));
  retired.source->push(device(Kind::DeviceAdded,3));Limits limits;limits.devices=2;limits.generations=1;retired.start(limits);
  batch=retired.poll();CHECK(batch.written==4&&batch.terminal&&batch.terminal->code==ErrorCode::Overflow);
  CHECK(retired.output[2].device.slot==1);CHECK(retired.events->status().value().closed);
  Fixture full;for(std::size_t i=0;i<17;++i)full.source->push(device(Kind::DeviceAdded,i+1));full.start();
  batch=full.poll();CHECK(batch.written==16&&batch.examined==17&&batch.terminal->code==ErrorCode::Exhausted);
  Fixture separate;separate.source->registered=2;
  separate.source->registrations[0]={DeviceKind::Keyboard,1};separate.source->registrations[1]={DeviceKind::Mouse,1};
  separate.source->push(key(1));auto mouse=key(1);mouse.kind=Kind::Motion;mouse.device_kind=DeviceKind::Mouse;mouse.key=Key::None;
  separate.source->push(mouse);separate.start();batch=separate.poll();
  CHECK(batch.written==2&&separate.output[0].device.slot!=separate.output[1].device.slot);
  Fixture initial;initial.source->registered=17;
  CHECK_ERROR(make_events(std::move(initial.pending),initial.clock,11),ErrorCode::Exhausted);
  Fixture bad;bad.source->registered=1;CHECK_ERROR(make_events(std::move(bad.pending),bad.clock,11),ErrorCode::InvalidInput);
}
void failure_and_clocks(){
  Fixture f;auto a=key();a.source_time=MonotonicTime{999,9};auto b=a;b.source_time->nanoseconds=1;
  f.source->push(a);f.source->push(b);f.source->push(key());f.source->fail_at=3;f.start();
  auto batch=f.poll();CHECK(batch.written==3&&batch.examined==3&&batch.terminal->code==ErrorCode::IoFailure);
  CHECK(f.output[0].delivered.domain==7&&f.output[0].source_time->domain==9);
  CHECK(f.output[1].source_time->nanoseconds==1&&!f.output[2].source_time);
  CHECK(f.output[2].service==11&&f.output[2].sequence==3);
  CHECK_ERROR(f.events->poll(f.output),ErrorCode::Stopping);
  for(bool domain:{false,true}){
    Fixture clock;clock.start();if(domain)clock.clock.time.domain=8;else clock.clock.time.nanoseconds=99;
    batch=clock.poll();CHECK(batch.terminal->code==ErrorCode::InvalidInput&&batch.examined==0&&clock.source->calls==0);
  }
  for(bool domain:{false,true}){
    Fixture mid;mid.source->push(key());mid.source->push(key());mid.source->change_clock=&mid.clock;
    mid.source->change_to=domain?MonotonicTime{100,8}:MonotonicTime{99,7};mid.start();
    batch=mid.poll();CHECK(batch.written==1&&batch.examined==2&&batch.terminal->code==ErrorCode::InvalidInput);
    CHECK(mid.output[0].sequence==1&&mid.events->status().value().closed);
  }
  for(auto source_domain:{0ULL,7ULL}){
    Fixture malformed;auto raw=key();raw.source_time=MonotonicTime{0,source_domain};malformed.source->push(raw);malformed.start();
    batch=malformed.poll();CHECK(batch.written==0&&batch.examined==1&&batch.terminal->code==ErrorCode::InvalidInput);
  }
  Fixture sequence;sequence.source->push(key());sequence.source->push(key());Limits limits;limits.sequences=1;sequence.start(limits);
  batch=sequence.poll();CHECK(batch.written==1&&batch.examined==2&&batch.terminal->code==ErrorCode::Overflow);
  Fixture epoch;epoch.source->push(lifecycle(Kind::FocusLost));limits={};limits.epochs=1;epoch.start(limits);
  batch=epoch.poll();CHECK(batch.written==0&&batch.terminal->code==ErrorCode::Overflow);
  Fixture thread;thread.start();std::thread other([&]{
    CHECK_ERROR(thread.events->poll(thread.output),ErrorCode::WrongThread);
    CHECK_ERROR(thread.events->status(),ErrorCode::WrongThread);
    CHECK_ERROR(thread.events->close(),ErrorCode::WrongThread);
  });other.join();CHECK(thread.source->calls==0&&!thread.events->status().value().closed);
  Fixture zero;CHECK_ERROR(make_events(std::move(zero.pending),zero.clock,0),ErrorCode::InvalidInput);
  Fixture allocation;bool destroyed=false;allocation.source->destroyed=&destroyed;
  desktop::profile::testing::fail_next_allocation_attempts(1);
  CHECK_ERROR(make_events(std::move(allocation.pending),allocation.clock,11),ErrorCode::Exhausted);
  CHECK(destroyed);desktop::profile::testing::fail_next_allocation_attempts(0);
}
void malformed(){
  std::array<SourceEvent,9> bad;bad.fill(key());
  bad[0].device_kind=DeviceKind::Mouse;bad[1].provenance=Provenance::Touch;
  bad[2].key=Key::None;bad[3].instance=9;bad[4].kind=Kind::Wheel;
  bad[5]=lifecycle(Kind::FocusLost);bad[5].provenance=Provenance::Registered;
  bad[6].x=std::numeric_limits<double>::infinity();bad[7].modifiers=16;
  bad[8]=device(Kind::DeviceAdded,0);
  for(auto raw:bad){Fixture f;f.source->push(key());f.source->push(raw);f.start();auto b=f.poll();
    CHECK(b.written==1&&b.examined==2&&b.terminal&&b.terminal->code==ErrorCode::InvalidInput);}
}
void navigation_cancellation(){
  for(auto kind:{Kind::FocusLost,Kind::Minimized,Kind::DeviceRemoved}){
    Fixture f;f.source->registrations[0]={DeviceKind::Keyboard,1};f.source->registered=1;
    f.source->push(key(1));f.source->push(kind==Kind::DeviceRemoved?device(kind,1):lifecycle(kind));
    auto repeat=key();repeat.repeat=true;f.source->push(repeat);f.start();
    desktop::NavigationInput navigation;CHECK(navigation.reset(100,true));
    for(std::size_t i=0;i<3;++i){auto batch=f.poll(1);CHECK(batch.written==1);const auto& event=f.output[0];
      if(event.kind==Kind::Key)CHECK(navigation.push({navigation.context(),event.delivered.nanoseconds,desktop::NavigationAction::Forward,event.pressed,event.repeat}));
      else CHECK(navigation.reset(event.delivered.nanoseconds,true));
      auto sample=navigation.sample(100,i+1);CHECK(sample);CHECK(sample.value().forward==(i==0?1:0));
    }
  }
}
void replay(){
  // Fixed 1024-record replay: one ignored record per four, no source allocation.
  // Timings are observations; only exact counts and allocation requests are gates.
  using Steady=std::chrono::steady_clock;
  std::array<double,11> elapsed{};std::uint64_t requests=0,bytes=0;
  for(std::size_t trial=0;trial<elapsed.size();++trial){
    Fixture f;for(std::size_t i=0;i<1024;++i)f.source->push(i%4==0?SourceEvent{}:key());f.start();
    CHECK(desktop::profile::reset_allocation_counters());std::size_t written=0,examined=0;
    const auto start=Steady::now();{
      desktop::profile::AllocationScope scope;
      for(;;){auto b=f.poll();CHECK(!b.terminal);written+=b.written;examined+=b.examined;if(b.observed_empty)break;}
      CHECK(f.events->close());
    }
    elapsed[trial]=std::chrono::duration<double,std::micro>(Steady::now()-start).count();
    auto allocations=desktop::profile::allocation_snapshot();CHECK(!allocations.overflowed);
    requests+=allocations.requests;bytes+=allocations.requested_bytes;
    CHECK(written==768&&examined==1024&&f.source->calls==1025);
  }
  CHECK(requests==0&&bytes==0);
  std::printf("{\"workload\":\"fixed_1024_input_records\",\"warmup_trials\":2,\"measured_trials\":9,\"written_per_trial\":768,\"allocation_requests\":%llu,\"microseconds\":[",
      static_cast<unsigned long long>(requests));
  for(std::size_t i=2;i<elapsed.size();++i)std::printf("%s%.3f",i==2?"":",",elapsed[i]);
  std::puts("]}");
}
}
int main(){bounds();identities();failure_and_clocks();malformed();navigation_cancellation();replay();}
