#include "race/platform/input.hpp"
#include <array>
#include <cmath>
#include <thread>

namespace race::platform::input {
namespace {
class PolledEvents final : public Events {
  struct Slot { Registration registration{};std::uint64_t generation{};bool active{}; };
  const std::thread::id owner_{std::this_thread::get_id()};
  std::unique_ptr<Source> source_;
  Clock& clock_;
  const Limits limits_;
  std::array<Slot,device_capacity> devices_{};
  MonotonicTime last_{};
  Status status_;
  bool owner() const noexcept {return owner_==std::this_thread::get_id();}
  Error stop(Error error) noexcept {status_.closed=true;status_.terminal=error;return error;}
  Result<MonotonicTime> now() noexcept {
    const auto time=clock_.now();
    if(!time.domain||time.domain!=last_.domain||time.nanoseconds<last_.nanoseconds)
      return stop(fail(ErrorCode::InvalidInput,"Input delivery clock changed or moved backwards"));
    last_=time;return time;
  }
  std::optional<std::size_t> find(Registration registration) const noexcept {
    for(std::size_t i=0;i<limits_.devices;++i)if(devices_[i].active&&
        devices_[i].registration.kind==registration.kind&&devices_[i].registration.instance==registration.instance)return i;
    return {};
  }
  Result<std::size_t> add(Registration registration) noexcept {
    if(registration.kind>DeviceKind::Mouse||!registration.instance)return fail(ErrorCode::InvalidInput,"Invalid input device registration");
    if(auto found=find(registration))return *found;
    bool retired=false;
    for(std::size_t i=0;i<limits_.devices;++i)if(!devices_[i].active){
      auto& slot=devices_[i];
      if(slot.generation==limits_.generations){retired=true;continue;}
      slot.registration=registration;++slot.generation;slot.active=true;++status_.devices;return i;
    }
    if(retired)return fail(ErrorCode::Overflow,"Input device generations exhausted");
    return fail(ErrorCode::Exhausted,"Input device table exhausted");
  }
  Result<void> advance_epoch() noexcept {
    if(status_.epoch==limits_.epochs)return fail(ErrorCode::Overflow,"Input cancellation epochs exhausted");
    ++status_.epoch;return {};
  }
public:
  PolledEvents(std::unique_ptr<Source> source,Clock& clock,std::uint64_t service,Limits limits)
      :source_(std::move(source)),clock_(clock),limits_(limits),last_(clock.now()),status_{service,1,0,0,false,{}}{}
  Result<void> initialize() noexcept {
    if(!last_.domain)return fail(ErrorCode::InvalidInput,"Input clock requires a domain");
    const auto initial=source_->initial_devices();
    if(initial.size()>limits_.devices)return fail(ErrorCode::Exhausted,"Initial input device table exhausted");
    for(const auto& device:initial){auto result=add(device);if(!result)return result.error();}
    return {};
  }
  Result<Status> status() const noexcept override {
    if(!owner())return fail(ErrorCode::WrongThread,"Input observation requires owner thread");return status_;
  }
  Result<void> close() noexcept override {
    if(!owner())return fail(ErrorCode::WrongThread,"Input close requires owner thread");
    if(!status_.closed){status_.closed=true;if(status_.epoch<limits_.epochs)++status_.epoch;}
    return {};
  }
  Result<Batch> poll(std::span<Event> output,std::size_t budget) noexcept override {
    if(!owner())return fail(ErrorCode::WrongThread,"Input poll requires owner thread");
    if(status_.closed)return fail(ErrorCode::Stopping,"Input service is closed");
    if(output.empty()||output.size()>batch_capacity||!budget||budget>work_capacity)
      return fail(ErrorCode::InvalidInput,"Invalid input batch or work limit");
    Batch batch;
    auto clock_check=now();if(!clock_check){batch.terminal=clock_check.error();return batch;}
    while(batch.written<output.size()&&batch.examined<budget){
      auto read=source_->next();
      if(!read){batch.terminal=stop(read.error());break;}
      if(!read.value()){batch.observed_empty=true;break;}
      ++batch.examined;const auto& raw=*read.value();
      if(raw.kind==Kind::Ignored){++batch.ignored;continue;}
      if(raw.kind>Kind::DeviceRemoved||raw.device_kind>DeviceKind::Mouse||raw.provenance>Provenance::Pen||
          raw.key>Key::Escape||raw.modifiers>15||!std::isfinite(raw.x)||!std::isfinite(raw.y)||
          (raw.source_time&&(!raw.source_time->domain||raw.source_time->domain==last_.domain))){
        batch.terminal=stop(fail(ErrorCode::InvalidInput,"Malformed normalized input event"));break;
      }
      const bool device_event=raw.kind==Kind::DeviceAdded||raw.kind==Kind::DeviceRemoved;
      const bool control_event=raw.kind==Kind::Key||raw.kind==Kind::RightButton||raw.kind==Kind::Motion||raw.kind==Kind::Wheel;
      const bool keyboard=raw.kind==Kind::Key;
      if((keyboard&&(raw.device_kind!=DeviceKind::Keyboard||raw.key==Key::None||raw.provenance>Provenance::Unknown))||
          (control_event&&!keyboard&&raw.device_kind!=DeviceKind::Mouse)||
          (control_event&&raw.provenance!=Provenance::Registered&&raw.instance)||
          (!control_event&&!device_event&&(raw.provenance!=Provenance::Unknown||raw.instance))){
        batch.terminal=stop(fail(ErrorCode::InvalidInput,"Input event kind and identity disagree"));break;
      }
      Device device{raw.device_kind,raw.provenance,0,0};
      if(device_event||(control_event&&raw.provenance==Provenance::Registered)){
        if(raw.provenance!=Provenance::Registered||!raw.instance){batch.terminal=stop(fail(ErrorCode::InvalidInput,"Invalid registered input identity"));break;}
        std::optional<std::size_t> slot=find({raw.device_kind,raw.instance});
        if(raw.kind==Kind::DeviceAdded){
          auto added=add({raw.device_kind,raw.instance});if(!added){batch.terminal=stop(added.error());break;}slot=added.value();
        }
        if(!slot){++batch.ignored;++batch.stale_devices;continue;}
        device.slot=static_cast<std::uint32_t>(*slot);device.generation=devices_[*slot].generation;
        if(raw.kind==Kind::DeviceRemoved){devices_[*slot].active=false;--status_.devices;}
      }
      if(raw.kind==Kind::FocusLost||raw.kind==Kind::Minimized||raw.kind==Kind::DeviceRemoved){
        auto advanced=advance_epoch();if(!advanced){batch.terminal=stop(advanced.error());break;}
      }
      if(status_.sequence==limits_.sequences){batch.terminal=stop(fail(ErrorCode::Overflow,"Input event sequences exhausted"));break;}
      auto delivered=now();if(!delivered){batch.terminal=delivered.error();break;}
      output[batch.written++]={raw.kind,status_.service,status_.epoch,++status_.sequence,
          delivered.value(),raw.source_time,device,raw.key,raw.modifiers,raw.pressed,raw.repeat,raw.x,raw.y};
    }
    batch.output_full=batch.written==output.size();batch.work_limit_reached=batch.examined==budget;return batch;
  }
};
} // namespace
Result<std::unique_ptr<Events>> make_events(std::unique_ptr<Source> source,Clock& clock,
    std::uint64_t service,const Limits& limits){
  if(!source||!service||!limits.devices||limits.devices>device_capacity||!limits.sequences||!limits.epochs||!limits.generations)
    return fail(ErrorCode::InvalidInput,"Invalid input service configuration");
  try{
    auto events=std::make_unique<PolledEvents>(std::move(source),clock,service,limits);
    auto initialized=events->initialize();if(!initialized)return initialized.error();
    return std::unique_ptr<Events>(std::move(events));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Input service allocation failed");}
}
} // namespace race::platform::input
