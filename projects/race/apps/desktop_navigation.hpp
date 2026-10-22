#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace race::desktop {
enum class NavigationAction : std::uint8_t { Left,Right,Forward,Backward,Down,Up,Count };
struct NavigationEvent {
  std::uint64_t context{},delivered_ns{};
  NavigationAction action{};
  bool pressed{},repeat{};
};
struct NavigationSample {
  std::uint64_t context{},update{},sampled_ns{};
  double right{},forward{},up{};
};
// Owner-thread application camera input. Delivery timestamps use the shared
// monotonic clock chosen by composition, not native device time. All reset,
// event and sample stamps in a context must use that same clock domain.
// The helper below supplies the Qt/application steady-clock domain.
inline std::uint64_t navigation_timestamp() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
class NavigationInput {
public:
  static constexpr std::size_t capacity=32;
private:
  std::array<NavigationEvent,capacity> events_{};
  std::array<bool,static_cast<std::size_t>(NavigationAction::Count)> held_{};
  std::size_t count_{};
  std::uint64_t context_{1},last_event_{},last_sample_{},last_update_{};
  bool active_{},overflowed_{};
  void clear() noexcept {held_.fill(false);count_=0;}
public:
  std::uint64_t context()const noexcept{return context_;}
  bool active()const noexcept{return active_;}
  bool overflowed()const noexcept{return overflowed_;} // Sticky observation.
  std::size_t pending()const noexcept{return count_;}
  Result<void> reset(std::uint64_t now,bool active) noexcept {
    // Cancellation is effective even when the caller's time is invalid or the
    // context ID is exhausted. Neither condition can leave a key held.
    clear();active_=false;
    if(context_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Navigation contexts exhausted");
    ++context_;
    if(now<last_sample_||now<last_event_)return fail(ErrorCode::InvalidInput,"Navigation reset time moved backwards");
    last_event_=last_sample_=now;active_=active;return {};
  }
  Result<void> push(NavigationEvent event) noexcept {
    if(event.context!=context_)return fail(ErrorCode::StaleHandle,"Navigation event context expired");
    if(!active_)return fail(ErrorCode::Cancelled,"Navigation context is inactive");
    if(event.action>=NavigationAction::Count)return fail(ErrorCode::InvalidInput,"Unknown navigation action");
    if(event.delivered_ns<last_event_||event.delivered_ns<last_sample_)return fail(ErrorCode::InvalidInput,"Navigation event time moved backwards");
    // Auto-repeat carries no new transition, including a repeat after reset.
    if(event.repeat){last_event_=event.delivered_ns;return {};}
    if(count_==capacity){
      clear();active_=false;overflowed_=true;
      if(context_!=std::numeric_limits<std::uint64_t>::max())++context_;
      return fail(ErrorCode::QueueFull,"Navigation queue overflow cancelled held input");
    }
    events_[count_++]=event;last_event_=event.delivered_ns;return {};
  }
  Result<NavigationSample> sample(std::uint64_t now,std::uint64_t update) noexcept {
    if(now<last_sample_||!update||update<=last_update_)return fail(ErrorCode::InvalidInput,"Navigation sample time/update is stale");
    std::size_t consumed=0;
    while(consumed<count_&&events_[consumed].delivered_ns<=now){
      const auto& event=events_[consumed++];held_[static_cast<std::size_t>(event.action)]=event.pressed;
    }
    for(std::size_t i=consumed;i<count_;++i)events_[i-consumed]=events_[i];
    count_-=consumed;last_sample_=now;last_update_=update;
    const auto pressed=[this](NavigationAction action){return held_[static_cast<std::size_t>(action)]?1.0:0.0;};
    NavigationSample out{context_,update,now};
    if(active_){
      out.right=pressed(NavigationAction::Right)-pressed(NavigationAction::Left);
      out.forward=pressed(NavigationAction::Forward)-pressed(NavigationAction::Backward);
      out.up=pressed(NavigationAction::Up)-pressed(NavigationAction::Down);
      const double length=std::sqrt(out.right*out.right+out.forward*out.forward+out.up*out.up);
      if(length>1){out.right/=length;out.forward/=length;out.up/=length;}
    }
    return out;
  }
};
}
