#include "sdl_input.hpp"
#include <array>

namespace race::platform::input {
namespace {
class SdlSource final : public Source {
  SDL_WindowID window_{};
  std::array<Registration,device_capacity> initial_{};
  std::size_t count_{};
  static Key key(SDL_Scancode code) noexcept {
    switch(code){
      case SDL_SCANCODE_A:return Key::A;case SDL_SCANCODE_D:return Key::D;
      case SDL_SCANCODE_W:return Key::W;case SDL_SCANCODE_S:return Key::S;
      case SDL_SCANCODE_Q:return Key::Q;case SDL_SCANCODE_E:return Key::E;
      case SDL_SCANCODE_ESCAPE:return Key::Escape;default:return Key::None;
    }
  }
  static void mouse(SourceEvent& out,SDL_MouseID id) noexcept {
    out.device_kind=DeviceKind::Mouse;
    if(id==SDL_TOUCH_MOUSEID)out.provenance=Provenance::Touch;
    else if(id==SDL_PEN_MOUSEID)out.provenance=Provenance::Pen;
    else if(!id)out.provenance=Provenance::Unknown;
    else {out.provenance=Provenance::Registered;out.instance=id;}
  }
public:
  explicit SdlSource(SDL_WindowID window):window_(window){}
  Result<void> initialize() noexcept {
    int count{};
    auto* keyboards=SDL_GetKeyboards(&count);
    if(!keyboards)return fail(ErrorCode::PlatformFailure,"SDL keyboard enumeration failed");
    if(count<0||static_cast<std::size_t>(count)>initial_.size()){
      SDL_free(keyboards);return fail(ErrorCode::Exhausted,"SDL keyboard device capacity exceeded");
    }
    for(int i=0;i<count;++i)initial_[count_++]={DeviceKind::Keyboard,keyboards[i]};
    SDL_free(keyboards);
    auto* mice=SDL_GetMice(&count);
    if(!mice)return fail(ErrorCode::PlatformFailure,"SDL mouse enumeration failed");
    if(count<0||static_cast<std::size_t>(count)>initial_.size()-count_){
      SDL_free(mice);return fail(ErrorCode::Exhausted,"SDL mouse device capacity exceeded");
    }
    for(int i=0;i<count;++i)initial_[count_++]={DeviceKind::Mouse,mice[i]};
    SDL_free(mice);return {};
  }
  std::span<const Registration> initial_devices() const noexcept override{return {initial_.data(),count_};}
  Result<std::optional<SourceEvent>> next() noexcept override {
    if(!SDL_GetWindowFromID(window_))return fail(ErrorCode::Stopping,"SDL input window no longer exists");
    SDL_Event event;
    if(!SDL_PollEvent(&event))return std::optional<SourceEvent>{};
    SourceEvent out;
    if(event.common.timestamp)out.source_time=MonotonicTime{event.common.timestamp,sdl_event_clock_domain};
    if(event.type==SDL_EVENT_QUIT)out.kind=Kind::Quit;
    else if(event.type>=SDL_EVENT_WINDOW_FIRST&&event.type<=SDL_EVENT_WINDOW_LAST&&event.window.windowID==window_){
      switch(event.type){
        case SDL_EVENT_WINDOW_FOCUS_LOST:out.kind=Kind::FocusLost;break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:out.kind=Kind::FocusGained;break;
        case SDL_EVENT_WINDOW_MINIMIZED:out.kind=Kind::Minimized;break;
        case SDL_EVENT_WINDOW_RESTORED:case SDL_EVENT_WINDOW_MAXIMIZED:out.kind=Kind::Restored;break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:out.kind=Kind::Resized;break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:out.kind=Kind::Close;break;
        default:break;
      }
    }else if((event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP)&&event.key.windowID==window_){
      out.key=key(event.key.scancode);
      if(out.key!=Key::None){
        out.kind=Kind::Key;out.pressed=event.type==SDL_EVENT_KEY_DOWN;out.repeat=event.key.repeat;
        out.device_kind=DeviceKind::Keyboard;out.instance=event.key.which;
        out.provenance=out.instance?Provenance::Registered:Provenance::Unknown;
        if(event.key.mod&SDL_KMOD_SHIFT)out.modifiers|=Shift;
        if(event.key.mod&SDL_KMOD_CTRL)out.modifiers|=Control;
        if(event.key.mod&SDL_KMOD_ALT)out.modifiers|=Alt;
        if(event.key.mod&SDL_KMOD_GUI)out.modifiers|=Gui;
      }
    }else if((event.type==SDL_EVENT_MOUSE_BUTTON_DOWN||event.type==SDL_EVENT_MOUSE_BUTTON_UP)&&
        event.button.windowID==window_&&event.button.button==SDL_BUTTON_RIGHT){
      out.kind=Kind::RightButton;out.pressed=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN;mouse(out,event.button.which);
    }else if(event.type==SDL_EVENT_MOUSE_MOTION&&event.motion.windowID==window_){
      out.kind=Kind::Motion;out.x=event.motion.xrel;out.y=event.motion.yrel;mouse(out,event.motion.which);
    }else if(event.type==SDL_EVENT_MOUSE_WHEEL&&event.wheel.windowID==window_){
      out.kind=Kind::Wheel;const double sign=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-1.0:1.0;
      out.x=event.wheel.x*sign;out.y=event.wheel.y*sign;mouse(out,event.wheel.which);
    }else if(event.type==SDL_EVENT_KEYBOARD_ADDED||event.type==SDL_EVENT_KEYBOARD_REMOVED){
      out.kind=event.type==SDL_EVENT_KEYBOARD_ADDED?Kind::DeviceAdded:Kind::DeviceRemoved;
      out.device_kind=DeviceKind::Keyboard;out.provenance=Provenance::Registered;out.instance=event.kdevice.which;
    }else if(event.type==SDL_EVENT_MOUSE_ADDED||event.type==SDL_EVENT_MOUSE_REMOVED){
      out.kind=event.type==SDL_EVENT_MOUSE_ADDED?Kind::DeviceAdded:Kind::DeviceRemoved;
      out.device_kind=DeviceKind::Mouse;out.provenance=Provenance::Registered;out.instance=event.mdevice.which;
    }
    return std::optional<SourceEvent>{out};
  }
};
}
Result<std::unique_ptr<Events>> make_sdl_events(SDL_Window* window,Clock& clock,std::uint64_t service,const Limits& limits){
  if(!SDL_IsMainThread())return fail(ErrorCode::WrongThread,"SDL input creation requires the main thread");
  if(!window)return fail(ErrorCode::InvalidInput,"SDL input requires a window");
  const auto id=SDL_GetWindowID(window);if(!id)return fail(ErrorCode::InvalidInput,"SDL input window identity missing");
  try{
    auto source=std::make_unique<SdlSource>(id);auto initialized=source->initialize();if(!initialized)return initialized.error();
    return make_events(std::move(source),clock,service,limits);
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"SDL input allocation failed");}
}
} // namespace race::platform::input
