#include "sdl_input.hpp"
#include "test.hpp"
#include <array>
#include <cstdio>
#include <thread>
using namespace race;
using namespace race::platform;
using namespace race::platform::input;
namespace {
struct TestClock final : Clock {MonotonicTime now() const noexcept override{return {1234,2};}};
void push(SDL_Event event){CHECK(SDL_PushEvent(&event));}
void flush(){SDL_PumpEvents();SDL_FlushEvents(SDL_EVENT_FIRST,SDL_EVENT_LAST);}
}
int main(){
  CHECK(SDL_Init(SDL_INIT_VIDEO));
  auto* window=SDL_CreateWindow("RACE input translation test",64,64,SDL_WINDOW_HIDDEN);CHECK(window);
  TestClock clock;CHECK_ERROR(make_sdl_events(nullptr,clock,1),ErrorCode::InvalidInput);
  auto created=make_sdl_events(window,clock,1);CHECK(created);auto events=std::move(created).value();
  auto storage=std::make_unique<std::array<Event,batch_capacity>>();
  std::span<Event> output{*storage};flush();
  const auto window_id=SDL_GetWindowID(window);
  SDL_Event key{};key.type=SDL_EVENT_KEY_DOWN;key.key.windowID=window_id;key.key.which=0;
  key.key.scancode=SDL_SCANCODE_W;key.key.key=SDLK_A;key.key.mod=SDL_KMOD_LSHIFT|SDL_KMOD_RCTRL;key.key.repeat=true;
  key.key.timestamp=888;push(key);
  SDL_Event motion{};motion.type=SDL_EVENT_MOUSE_MOTION;motion.motion.windowID=window_id;
  motion.motion.which=SDL_TOUCH_MOUSEID;motion.motion.xrel=3;motion.motion.yrel=-4;push(motion);
  SDL_Event wheel{};wheel.type=SDL_EVENT_MOUSE_WHEEL;wheel.wheel.windowID=window_id;
  wheel.wheel.which=SDL_PEN_MOUSEID;wheel.wheel.x=2;wheel.wheel.y=-3;wheel.wheel.direction=SDL_MOUSEWHEEL_FLIPPED;push(wheel);
  SDL_Event button{};button.type=SDL_EVENT_MOUSE_BUTTON_DOWN;button.button.windowID=window_id;
  button.button.button=SDL_BUTTON_RIGHT;push(button);
  SDL_Event focus{};focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;focus.window.windowID=window_id;push(focus);
  SDL_Event removed{};removed.type=SDL_EVENT_KEYBOARD_REMOVED;removed.kdevice.which=0x7ffffff0;push(removed);
  // SDL_PushEvent copies values but does not mutate physical key/focus/device state.
  auto polled=events->poll(output);CHECK(polled);auto batch=polled.value();
  CHECK(!batch.terminal&&batch.written==5&&batch.stale_devices==1);
  CHECK(output[0].kind==Kind::Key&&output[0].key==Key::W&&output[0].repeat&&output[0].pressed);
  CHECK(output[0].modifiers==(Shift|Control)&&output[0].device.provenance==Provenance::Unknown);
  CHECK(output[0].source_time&&output[0].source_time->nanoseconds==888&&output[0].source_time->domain==sdl_event_clock_domain);
  CHECK(output[0].delivered.nanoseconds==1234&&output[0].delivered.domain==2);
  CHECK(output[1].kind==Kind::Motion&&output[1].device.provenance==Provenance::Touch&&output[1].x==3&&output[1].y==-4);
  CHECK(output[2].kind==Kind::Wheel&&output[2].device.provenance==Provenance::Pen&&output[2].x==-2&&output[2].y==3);
  CHECK(output[3].kind==Kind::RightButton&&output[3].pressed);
  CHECK(output[4].kind==Kind::FocusLost&&output[4].epoch==2&&output[4].device.provenance==Provenance::Unknown);
  flush();
  key.key.repeat=false;key.key.windowID=window_id+100;push(key); // Foreign window is consumed, never delivered.
  key.key.windowID=window_id;key.key.scancode=SDL_SCANCODE_F1;push(key);
  button.button.button=SDL_BUTTON_LEFT;push(button);
  motion.motion.windowID=window_id+100;push(motion);
  focus.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED;focus.window.windowID=window_id+100;push(focus);
  for(int i=0;i<1019;++i){SDL_Event ignored{};ignored.type=SDL_EVENT_USER;push(ignored);}
  key.key.scancode=SDL_SCANCODE_A;push(key);
  polled=events->poll(output);CHECK(polled);batch=polled.value();
  CHECK(batch.examined==1024&&batch.ignored==1024&&batch.work_limit_reached&&batch.written==0&&!batch.terminal);
  polled=events->poll(output);CHECK(polled&&polled.value().written==1&&output[0].key==Key::A);
  flush();for(int i=0;i<129;++i)push(key);
  polled=events->poll(output);CHECK(polled&&polled.value().written==128&&polled.value().output_full);
  polled=events->poll(output);CHECK(polled&&polled.value().written==1);
  flush();
  const auto devices=events->status().value().devices;
  if(devices<device_capacity){
    SDL_Event added{};added.type=SDL_EVENT_KEYBOARD_ADDED;added.kdevice.which=0x7ffffff0;push(added);
    key.key.which=added.kdevice.which;push(key);push(removed);push(key);
    polled=events->poll(output);CHECK(polled);batch=polled.value();
    CHECK(batch.written==3&&batch.stale_devices==1&&!batch.terminal);
    CHECK(output[0].device==output[1].device&&output[1].device==output[2].device);
    CHECK(output[1].device.provenance==Provenance::Registered&&output[1].device.generation!=0);
  }
  flush();SDL_Event quit{};quit.type=SDL_EVENT_QUIT;push(quit);
  polled=events->poll(output);CHECK(polled&&polled.value().written==1&&output[0].kind==Kind::Quit);
  std::thread wrong([&]{CHECK_ERROR(make_sdl_events(window,clock,2),ErrorCode::WrongThread);
    CHECK_ERROR(events->poll(output),ErrorCode::WrongThread);});wrong.join();
  CHECK(events->close());CHECK(events->close());CHECK_ERROR(events->poll(output),ErrorCode::Stopping);
  events.reset();
  auto lifetime=make_sdl_events(window,clock,2);CHECK(lifetime);
  SDL_DestroyWindow(window);
  polled=lifetime.value()->poll(output);CHECK(polled&&polled.value().terminal&&polled.value().terminal->code==ErrorCode::Stopping);
  lifetime.value().reset();SDL_Quit();
  std::printf("SDL input copied-event translation passed; initial enumerated devices: %zu; no physical hotplug claim\n",devices);
}
