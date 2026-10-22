#include "../apps/desktop_navigation.hpp"
#include "test.hpp"
#include <cmath>
using namespace race;
using namespace race::desktop;
int main(){
  NavigationInput input;
  CHECK_ERROR(input.push({input.context(),0,NavigationAction::Right,true,false}),ErrorCode::Cancelled);
  CHECK(input.reset(10,true));const auto first=input.context();
  CHECK(input.push({first,12,NavigationAction::Right,true,false}));
  CHECK(input.sample(11,1).value().right==0);CHECK(input.pending()==1);
  CHECK(input.sample(12,2).value().right==1);
  CHECK(input.push({first,13,NavigationAction::Left,true,false}));
  CHECK(input.sample(13,3).value().right==0);
  CHECK(input.push({first,14,NavigationAction::Left,false,false}));
  CHECK(input.push({first,14,NavigationAction::Forward,true,false}));
  auto diagonal=input.sample(14,4);CHECK(diagonal);
  CHECK(std::abs(diagonal.value().right-std::sqrt(.5))<1e-12);
  CHECK(std::abs(diagonal.value().forward-std::sqrt(.5))<1e-12);
  CHECK_ERROR(input.sample(13,5),ErrorCode::InvalidInput);
  CHECK_ERROR(input.sample(15,4),ErrorCode::InvalidInput);
  CHECK_ERROR(input.push({first,13,NavigationAction::Up,true,false}),ErrorCode::InvalidInput);
  CHECK_ERROR(input.push({first,15,NavigationAction::Count,true,false}),ErrorCode::InvalidInput);
  CHECK(input.reset(15,false));CHECK(input.pending()==0);
  CHECK(input.sample(16,5).value().right==0);
  CHECK_ERROR(input.push({first,17,NavigationAction::Right,false,false}),ErrorCode::StaleHandle);
  CHECK(input.reset(17,true));
  CHECK(input.push({input.context(),18,NavigationAction::Right,true,true}));
  CHECK(input.sample(18,6).value().right==0); // Repeat cannot revive focus-lost key.
  CHECK(input.push({input.context(),19,NavigationAction::Right,true,false}));
  CHECK(input.sample(19,7).value().right==1);
  CHECK(input.push({input.context(),20,NavigationAction::Right,false,false}));
  CHECK(input.sample(20,8).value().right==0);
  for(auto action:{NavigationAction::Right,NavigationAction::Forward,NavigationAction::Up})
    CHECK(input.push({input.context(),21,action,true,false}));
  auto held=input.sample(21,9);CHECK(held&&held.value().right>0&&held.value().forward>0&&held.value().up>0);
  for(std::size_t i=0;i<NavigationInput::capacity;++i)
    CHECK(input.push({input.context(),22,NavigationAction::Right,i%2==0,false}));
  CHECK_ERROR(input.push({input.context(),22,NavigationAction::Right,false,false}),ErrorCode::QueueFull);
  CHECK(input.overflowed()&&!input.active()&&input.pending()==0);
  auto cancelled=input.sample(22,10);CHECK(cancelled&&cancelled.value().right==0&&cancelled.value().forward==0&&cancelled.value().up==0);
  CHECK(input.reset(23,true));
  CHECK(input.push({input.context(),24,NavigationAction::Up,true,false}));
  CHECK(input.sample(24,11).value().up==1);
  CHECK_ERROR(input.reset(23,true),ErrorCode::InvalidInput);
  CHECK(!input.active()&&input.sample(25,12).value().up==0);
  CHECK(input.reset(26,true));
  for(std::size_t i=0;i<NavigationInput::capacity*2;++i)
    CHECK(input.push({input.context(),27,NavigationAction::Up,true,true}));
  CHECK(input.pending()==0&&input.sample(27,13).value().up==0);
  CHECK(input.push({input.context(),100,NavigationAction::Up,true,true}));
  CHECK_ERROR(input.push({input.context(),99,NavigationAction::Up,true,false}),ErrorCode::InvalidInput);
  return 0;
}
