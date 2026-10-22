#include "desktop_gizmo.hpp"
#include "test.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace race;
namespace g=race::desktop::gizmo;
namespace {
constexpr std::array<scene::Vec3,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
constexpr double pi=std::numbers::pi;
g::Point add(g::Point a,g::Point b){return {a.x+b.x,a.y+b.y};}
g::Point sub(g::Point a,g::Point b){return {a.x-b.x,a.y-b.y};}
g::Point mul(g::Point a,double s){return {a.x*s,a.y*s};}
double length(g::Point p){return std::hypot(p.x,p.y);}
void near(double a,double b,double tolerance=1e-9){CHECK(std::abs(a-b)<=tolerance);}
render::ViewportCamera front(int axis,double distance=10){
  render::ViewportCamera camera;camera.eye={};
  if(axis==0)camera.eye.x=distance;if(axis==1)camera.eye.y=distance;if(axis==2)camera.eye.z=distance;
  camera.target={};camera.up=axis==1?render::ViewportVec3{0,0,1}:render::ViewportVec3{0,1,0};return camera;
}
g::Layout make(g::Mode mode,render::ViewportCamera camera={},int active=-1){
  auto result=g::layout({},axes,camera,1000,760,mode,active);CHECK(result);return result.value();
}
void move_and_scale(){
  for(auto mode:{g::Mode::Move,g::Mode::Scale}){
    const auto l=make(mode);
    CHECK(!g::hit_test(l,{10,10}));CHECK(!g::hit_test(l,l.pivot));
    for(int i=0;i<3;++i){const auto& axis=l.axes[static_cast<std::size_t>(i)];CHECK(axis.visible);
      CHECK(g::hit_test(l,axis.end)==i);
      const auto delta=sub(axis.end,l.pivot);
      const auto start=add(l.pivot,mul(delta,.5));
      auto unchanged=g::drag_amount(l,start,start,i);CHECK(unchanged);near(*unchanged,mode==g::Mode::Scale?1.:0.);
      const auto forward=g::drag_amount(l,start,add(start,delta),i);
      const auto backward=g::drag_amount(l,start,sub(start,delta),i);
      CHECK(forward&&backward);
      if(mode==g::Mode::Move){CHECK(*forward>0&&*backward<0);
        const auto reverse=g::drag_amount(l,add(start,delta),start,i);CHECK(reverse);near(*reverse,-*forward);
      }else {near(*forward,2);near(*backward,.5);}
      // An off-axis pointer displacement must not turn into an axis movement.
      const g::Point perpendicular{-delta.y*.2,delta.x*.2};
      const auto transverse=g::drag_amount(l,start,add(start,perpendicular),i);CHECK(transverse);near(*transverse,mode==g::Mode::Scale?1.:0.);
    }
  }
  const auto l=make(g::Mode::Move,front(2));CHECK(l.axes[0].visible&&l.axes[1].visible&&l.axes[2].visible&&l.axes[2].end_on);
  const auto x=sub(l.axes[0].end,l.pivot);near(length(x),g::handle_pixels);
  const auto amount=g::drag_amount(l,l.pivot,l.axes[0].end,0);CHECK(amount);near(*amount,l.world_length);
  near(*g::drag_amount(l,l.pivot,l.pivot,2),0);
  const auto scale=make(g::Mode::Scale,front(2));const auto delta=sub(scale.axes[0].end,scale.pivot);
  near(*g::drag_amount(scale,scale.pivot,add(scale.pivot,mul(delta,100)),0),1024);
  near(*g::drag_amount(scale,scale.pivot,sub(scale.pivot,mul(delta,100)),0),1./1024);
}
void rotation(){
  for(int i=0;i<3;++i){
    const auto l=make(g::Mode::Rotate,front(i));const auto& ring=l.axes[static_cast<std::size_t>(i)];CHECK(ring.visible);
    for(int j=0;j<3;++j){CHECK(l.axes[static_cast<std::size_t>(j)].visible);CHECK(l.axes[static_cast<std::size_t>(j)].edge_on==(i!=j));}
    CHECK(g::hit_test(l,ring.ring[8])==i);CHECK(g::hit_test(l,l.pivot)!=i);
    const auto positive=g::drag_amount(l,ring.ring[0],ring.ring[16],i);
    const auto negative=g::drag_amount(l,ring.ring[16],ring.ring[0],i);
    CHECK(positive&&negative);near(*positive,pi/2);near(*negative,-pi/2);
    near(*g::drag_amount(l,ring.ring[7],ring.ring[7],i),0);
    // Crossing the 2pi seam must produce a small positive angle, not a jump.
    near(*g::drag_amount(l,ring.ring[63],ring.ring[1],i),pi/16);
    CHECK(!g::drag_amount(l,l.pivot,ring.ring[8],i));
    auto reversed=front(i);reversed.eye.x=-reversed.eye.x;reversed.eye.y=-reversed.eye.y;reversed.eye.z=-reversed.eye.z;
    const auto back=make(g::Mode::Rotate,reversed);const auto& r=back.axes[static_cast<std::size_t>(i)];
    CHECK(r.visible);near(*g::drag_amount(back,r.ring[0],r.ring[16],i),pi/2);
  }
  // Oblique perspective retains the axis-plane angle, rather than a screen atan2 angle.
  const auto oblique=make(g::Mode::Rotate);
  for(int i=0;i<3;++i){const auto& r=oblique.axes[static_cast<std::size_t>(i)];CHECK(r.visible);
    const auto angle=g::drag_amount(oblique,r.ring[3],r.ring[19],i);CHECK(angle);near(*angle,pi/2);
  }
  const auto zoomed=make(g::Mode::Rotate,front(2,100));
  near(length(sub(zoomed.axes[2].ring[0],zoomed.pivot)),g::handle_pixels);
  near(*g::drag_amount(zoomed,zoomed.axes[2].ring[0],zoomed.axes[2].ring[16],2),pi/2);
}
void camera_zoom_and_roll(){
  const auto near_view=make(g::Mode::Move,front(2,10)),far_view=make(g::Mode::Move,front(2,100));
  near(length(sub(near_view.axes[0].end,near_view.pivot)),g::handle_pixels);
  near(length(sub(far_view.axes[0].end,far_view.pivot)),g::handle_pixels);
  const auto a=g::drag_amount(near_view,near_view.pivot,add(near_view.pivot,{30,0}),0);
  const auto b=g::drag_amount(far_view,far_view.pivot,add(far_view.pivot,{30,0}),0);CHECK(a&&b);near(*b,*a*10);
  auto rolled=front(2);rolled.up={1,0,0};const auto roll=make(g::Mode::Move,rolled);
  near(roll.axes[0].end.x,roll.pivot.x);CHECK(roll.axes[0].end.y<roll.pivot.y);
}
void camera_affine_axes(){
  const auto near_view=make(g::Mode::Move,front(2,10));
  auto anisotropic=axes;anisotropic[0]={2,0,0};anisotropic[1]={1,3,0};
  auto transformed=g::layout({},anisotropic,front(2),1000,760,g::Mode::Move);CHECK(transformed);
  near(transformed.value().axes[0].end.x,near_view.axes[0].end.x);
  CHECK(transformed.value().axes[1].end.x>transformed.value().pivot.x&&transformed.value().axes[1].end.y<transformed.value().pivot.y);
}
void camera_large_precision(){
  const auto near_view=make(g::Mode::Move,front(2,10));
  // Camera-relative projection does not add tiny handles to a large absolute pivot.
  auto translated=front(2);translated.eye={1e9,-1e9,1e9+10};translated.target={1e9,-1e9,1e9};
  const auto large=g::layout({1e9,-1e9,1e9},axes,translated,1000,760,g::Mode::Move);CHECK(large);
  near(large.value().axes[0].end.x,near_view.axes[0].end.x);near(large.value().axes[1].end.y,near_view.axes[1].end.y);
  near(*g::drag_amount(large.value(),large.value().pivot,large.value().axes[0].end,0),near_view.world_length);
}
void camera_perspective_drag(){
  // Independently project a known world-axis displacement along a depth-varying axis.
  const auto perspective=make(g::Mode::Move);const auto& axis=perspective.axes[0];
  const auto point=[&](double metres){const auto p=scene::Vec3{perspective.pivot_relative.x+axis.direction.x*metres,perspective.pivot_relative.y+axis.direction.y*metres,perspective.pivot_relative.z+axis.direction.z*metres};
    const auto dot=[](scene::Vec3 u,scene::Vec3 v){return u.x*v.x+u.y*v.y+u.z*v.z;};const auto scale=perspective.height/(2*dot(p,perspective.forward)*perspective.tangent);
    return g::Point{perspective.width*.5+dot(p,perspective.right)*scale,perspective.height*.5-dot(p,perspective.up)*scale};};
  const auto from=perspective.world_length*.3,to=perspective.world_length*1.4;
  near(*g::drag_amount(perspective,point(from),point(to),0),to-from);
}
void transformed_rotation_planes(){
  const std::array<std::array<scene::Vec3,3>,3> bases{{
    {{{-2,0,0},{0,3,0},{0,0,1}}},
    {{{2,0,0},{0,.5,0},{0,0,3}}},
    {{{2,.25,0},{.75,1.5,.1},{.2,.6,2}}}
  }};
  const auto dot=[](scene::Vec3 a,scene::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto cross=[](scene::Vec3 a,scene::Vec3 b){return scene::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  for(const auto& basis:bases)for(int i=0;i<3;++i){
    const auto u=basis[static_cast<std::size_t>((i+1)%3)],v=basis[static_cast<std::size_t>((i+2)%3)],normal=cross(u,v);
    const double n=std::hypot(normal.x,normal.y,normal.z);CHECK(n>0);
    render::ViewportCamera camera;camera.eye={normal.x/n*10,normal.y/n*10,normal.z/n*10};camera.target={};
    camera.up=std::abs(normal.y/n)>.9?render::ViewportVec3{0,0,1}:render::ViewportVec3{0,1,0};
    auto made=g::layout({},basis,camera,1000,760,g::Mode::Rotate);CHECK(made);const auto& l=made.value();const auto& ring=l.axes[static_cast<std::size_t>(i)];CHECK(ring.visible);
    near(dot(ring.direction,normal)/n,1);
    // The projected plane is front-facing: unequal input lengths remain unequal
    // radii, and a shear retains the angle between its two local basis vectors.
    const auto screen_u=sub(ring.ring[0],l.pivot),screen_v=sub(ring.ring[16],l.pivot);
    near(length(screen_u)/length(screen_v),std::hypot(u.x,u.y,u.z)/std::hypot(v.x,v.y,v.z));
    near((screen_u.x*screen_v.x+screen_u.y*screen_v.y)/(length(screen_u)*length(screen_v)),dot(u,v)/(std::hypot(u.x,u.y,u.z)*std::hypot(v.x,v.y,v.z)));
    near(*g::drag_amount(l,ring.ring[0],ring.ring[16],i),pi/2);
    near(*g::drag_amount(l,ring.ring[16],ring.ring[0],i),-pi/2);
    near(*g::drag_amount(l,ring.ring[63],ring.ring[1],i),pi/16);
    // Repeat at an oblique camera angle, where a screen-space angle is wrong.
    camera.eye.x+=u.x*.7+v.x*.3;camera.eye.y+=u.y*.7+v.y*.3;camera.eye.z+=u.z*.7+v.z*.3;
    auto oblique=g::layout({},basis,camera,1000,760,g::Mode::Rotate);CHECK(oblique&&oblique.value().axes[static_cast<std::size_t>(i)].visible);
    const auto& r=oblique.value().axes[static_cast<std::size_t>(i)];near(*g::drag_amount(oblique.value(),r.ring[5],r.ring[21],i),pi/2);
  }
  auto singular=axes;singular[1]={2,0,0};auto made=g::layout({},singular,{},1000,760,g::Mode::Rotate);CHECK(made&&!made.value().axes[2].visible);
}
void overlay_reuse(){
  using Draws=std::vector<render::OverlayDraw>;
  const auto equal=[](const Draws& a,const Draws& b){
    CHECK(a.size()==b.size());for(std::size_t i=0;i<a.size();++i){
      CHECK(a[i].color==b[i].color);CHECK(a[i].vertices==b[i].vertices);
    }
  };
  constexpr std::array modes{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale};
  // Fixture storage is outside the retained overlay buffers under test.
  std::vector<g::Layout> layouts(12);std::array<Draws,12> expected;
  std::size_t index{};
  for(auto mode:modes)for(int active=-1;active<3;++active){
    layouts[index]=make(mode,{},active);auto fresh=g::overlays(layouts[index]);CHECK(fresh&&fresh.value().size()==3);
    expected[index++]=std::move(fresh).value();
  }
  Draws reused;CHECK(g::overlays(make(g::Mode::Rotate),reused));CHECK(reused.size()==3);
  const auto* outer=reused.data();const auto outer_capacity=reused.capacity();
  std::array<const std::array<float,2>*,3> storage{};std::array<std::size_t,3> capacities{};
  for(std::size_t i=0;i<reused.size();++i){storage[i]=reused[i].vertices.data();capacities[i]=reused[i].vertices.capacity();CHECK(capacities[i]>=g::ring_segments*6);}
  // Reorder the active axis and alternate all three differently sized shapes.
  // Exact content plus stable addresses/capacities catch stale tails and buffer
  // replacement, including the editor's frame ownership transfer and reclaim.
  for(int repeat=0;repeat<8;++repeat)for(std::size_t i=0;i<layouts.size();++i){
    CHECK(g::overlays(layouts[i],reused));equal(reused,expected[i]);
    CHECK(reused.data()==outer&&reused.capacity()==outer_capacity);
    for(std::size_t n=0;n<reused.size();++n)CHECK(reused[n].vertices.data()==storage[n]&&reused[n].vertices.capacity()==capacities[n]);
    render::RenderFrame frame;frame.overlays=std::move(reused);equal(frame.overlays,expected[i]);reused=std::move(frame.overlays);
  }
  // A reduced visible-axis count must erase every stale draw, while preserving
  // the surviving slot. Empty output and later recovery are valid transitions.
  auto one=make(g::Mode::Rotate,front(2));one.axes[0].visible=one.axes[1].visible=false;CHECK(g::overlays(one,reused));CHECK(reused.size()==1);
  CHECK(reused.data()==outer&&reused[0].vertices.data()==storage[0]);equal(reused,g::overlays(one).value());
  auto hidden=one;for(auto& axis:hidden.axes)axis.visible=false;
  CHECK(g::overlays(hidden,reused));CHECK(reused.empty()&&reused.capacity()==outer_capacity);
  CHECK(g::overlays(layouts[0],reused));equal(reused,expected[0]);
  const auto reject=[&](const g::Layout& bad){
    CHECK(g::overlays(layouts[0],reused));CHECK(!reused.empty());
    CHECK_ERROR(g::overlays(bad,reused),ErrorCode::InvalidInput);CHECK(reused.empty());
    CHECK(g::overlays(layouts[0],reused));equal(reused,expected[0]);
  };
  auto malformed=layouts[0];malformed.width=0;reject(malformed);
  malformed=layouts[0];malformed.mode=static_cast<g::Mode>(9);reject(malformed);
  malformed=layouts[0];malformed.active_axis=3;reject(malformed);
  // Failure after a valid first draw must remove that partial result too.
  malformed=layouts[0];malformed.axes[1].end.x=std::numeric_limits<double>::quiet_NaN();reject(malformed);
  malformed=layouts[0];malformed.axes[1].end=malformed.axes[1].start;reject(malformed);
  malformed=layouts[0];malformed.axes[1].end.x=malformed.width*20;reject(malformed);
  malformed=make(g::Mode::Rotate);malformed.axes[1].ring[30]=malformed.axes[1].ring[29];reject(malformed);
}
// Keep each rejected Result<Layout> in this helper's small frame; the caller
// supplies the same invalid cases without reserving ten large temporaries.
void invalid_layout(scene::Vec3 pivot,const std::array<scene::Vec3,3>& world_axes,
    const render::ViewportCamera& camera,double width,double height,g::Mode mode,int active=-1){
  CHECK_ERROR(g::layout(pivot,world_axes,camera,width,height,mode,active),ErrorCode::InvalidInput);
}
void validation_projection_inputs(){
  const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
  invalid_layout({nan,0,0},axes,{},1000,760,g::Mode::Move);
  invalid_layout({},axes,{},0,760,g::Mode::Move);
  invalid_layout({},axes,{},1000,inf,g::Mode::Move);
  invalid_layout({},axes,{},4097,760,g::Mode::Move);
  invalid_layout({},axes,{},1000,760,static_cast<g::Mode>(9));
  invalid_layout({},axes,{},1000,760,g::Mode::Move,3);
  auto bad_axes=axes;bad_axes[1].x=nan;invalid_layout({},bad_axes,{},1000,760,g::Mode::Move);
  auto camera=front(2);camera.target=camera.eye;invalid_layout({},axes,camera,1000,760,g::Mode::Move);
  camera=front(2);camera.up={0,0,1};invalid_layout({},axes,camera,1000,760,g::Mode::Move);
  camera=front(2);camera.vertical_fov_radians=nan;invalid_layout({},axes,camera,1000,760,g::Mode::Move);
  camera=front(2);camera.eye.x=.02;
  CHECK(make(g::Mode::Move,camera).axes[2].visible);
  CHECK(make(g::Mode::Rotate,camera).axes[0].visible);
}
void validation_hidden_axes(){
  auto zeros=axes;zeros[0]={};auto zero=g::layout({},zeros,front(2),1000,760,g::Mode::Move);CHECK(zero&&!zero.value().axes[0].visible);
  for(const auto pivot:{scene::Vec3{0,0,11},scene::Vec3{1000,0,0},scene::Vec3{0,0,9.99}}){
    auto hidden=g::layout(pivot,axes,front(2),1000,760,g::Mode::Move);CHECK(hidden);
    CHECK(std::none_of(hidden.value().axes.begin(),hidden.value().axes.end(),[](const auto& axis){return axis.visible;}));
    CHECK(g::overlays(hidden.value()).value().empty());CHECK(!g::hit_test(hidden.value(),{500,380}));
  }
}
void validation_pointer_input(){
  const auto nan=std::numeric_limits<double>::quiet_NaN(),inf=std::numeric_limits<double>::infinity();
  const auto l=make(g::Mode::Move);CHECK(!g::hit_test(l,{nan,0}));CHECK(!g::hit_test(l,{0,0},inf));
  CHECK(!g::drag_amount(l,{0,0},{inf,0},0));CHECK(!g::drag_amount(l,{0,0},{0,0},-1));
}
void validation_overlay_output(){
  for(auto mode:{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale}){
    const auto layout=make(mode,{},1);const auto output=g::overlays(layout);CHECK(output&&output.value().size()==3);
    std::size_t vertices=0;for(const auto& draw:output.value()){
      CHECK(!draw.vertices.empty()&&draw.vertices.size()%3==0);CHECK(draw.color[3]==1);vertices+=draw.vertices.size();
      for(const auto& p:draw.vertices)CHECK(std::isfinite(p[0])&&std::isfinite(p[1])&&std::abs(p[0])<=16&&std::abs(p[1])<=16);
    }
    CHECK(vertices<=1152&&vertices<4096);CHECK(output.value().back().color==std::array<float,4>{1,.72f,.12f,1});
    const auto again=g::overlays(layout);CHECK(again&&again.value().size()==output.value().size());
    for(std::size_t i=0;i<again.value().size();++i)CHECK(again.value()[i].vertices==output.value()[i].vertices);
  }
}
void validation_overlay_rejections(){
  const auto nan=std::numeric_limits<double>::quiet_NaN();const auto l=make(g::Mode::Move);
  auto malformed=l;malformed.axes[0].end.x=nan;CHECK_ERROR(g::overlays(malformed),ErrorCode::InvalidInput);
  malformed=l;malformed.axes[0].end.x=l.width*20;CHECK_ERROR(g::overlays(malformed),ErrorCode::InvalidInput);
}
void validation_active_axis_tie(){
  auto coincident=axes;coincident[1]=coincident[0];const auto tie=g::layout({},coincident,front(2),1000,760,g::Mode::Move,1);
  CHECK(tie&&g::hit_test(tie.value(),tie.value().axes[0].end)==1);
}
void validation_and_bounds(){
  validation_projection_inputs();validation_hidden_axes();validation_pointer_input();
  validation_overlay_output();validation_overlay_rejections();validation_active_axis_tie();
}
void true_projection(const g::Layout& l,const std::array<scene::Vec3,3>& basis,scene::Vec3 pivot={}){
  const auto dot3=[](scene::Vec3 a,scene::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto normalize=[](scene::Vec3 a){const auto n=std::hypot(a.x,a.y,a.z);return scene::Vec3{a.x/n,a.y/n,a.z/n};};
  const auto cross3=[](scene::Vec3 a,scene::Vec3 b){return scene::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};};
  const auto& c=l.camera;const auto f=normalize({c.target.x-c.eye.x,c.target.y-c.eye.y,c.target.z-c.eye.z});
  const auto r=normalize(cross3(f,{c.up.x,c.up.y,c.up.z})),u=cross3(r,f);
  const auto project=[&](scene::Vec3 offset){const scene::Vec3 relative{pivot.x-c.eye.x+offset.x,pivot.y-c.eye.y+offset.y,pivot.z-c.eye.z+offset.z};
    const double gain=l.height/(2*dot3(relative,f)*std::tan(c.vertical_fov_radians/2));
    return g::Point{l.width/2+dot3(relative,r)*gain,l.height/2-dot3(relative,u)*gain};};
  const auto center=project({});near(l.pivot.x,center.x);near(l.pivot.y,center.y);
  for(std::size_t i=0;i<3;++i){const auto& a=l.axes[i];if(!a.visible)continue;
    if(l.mode==g::Mode::Rotate){const auto raw_u=basis[(i+1)%3],raw_v=basis[(i+2)%3];
      const double scale=l.world_length/std::max(std::hypot(raw_u.x,raw_u.y,raw_u.z),std::hypot(raw_v.x,raw_v.y,raw_v.z));
      for(std::size_t n=0;n<g::ring_segments;++n){const double angle=2*pi*static_cast<double>(n)/g::ring_segments;
        const double x=std::cos(angle)*scale,y=std::sin(angle)*scale;
        const auto expected=project({raw_u.x*x+raw_v.x*y,raw_u.y*x+raw_v.y*y,raw_u.z*x+raw_v.z*y});
        near(a.ring[n].x,expected.x,1e-8);near(a.ring[n].y,expected.y,1e-8);
      }
    }else{const auto direction=normalize(basis[i]);const auto expected=project({direction.x*l.world_length,direction.y*l.world_length,direction.z*l.world_length});
      near(a.end.x,expected.x);near(a.end.y,expected.y);
      if(a.end_on){near(a.start.x,expected.x);near(a.start.y,expected.y);}
      else {const auto first=sub(a.start,center),last=sub(expected,center);near(first.x*last.y-first.y*last.x,0,1e-8);CHECK(first.x*last.x+first.y*last.y>0);}
    }
  }
}
void constrained_rotation(const g::Layout& l,int i){
  const auto& a=l.axes[static_cast<std::size_t>(i)];const auto start=a.ring[5];
  const auto direction=g::drag_direction(l,start,i);CHECK(direction);near(length(*direction),1);
  near(*g::drag_amount(l,start,add(start,mul(*direction,85*pi/2)),i),pi/2);
  near(*g::drag_amount(l,start,sub(start,mul(*direction,85*pi/2)),i),-pi/2);
  near(*g::drag_amount(l,start,start,i),0);
  const g::Point perpendicular{-direction->y,direction->x};near(*g::drag_amount(l,start,add(start,mul(perpendicular,50)),i),0);
  double accumulated=0,previous=0;
  for(int n=1;n<=160;++n){const double angle=n*2*pi/64;const auto amount=g::drag_amount(l,start,add(start,mul(*direction,85*angle)),i);CHECK(amount);
    accumulated+=std::remainder(*amount-previous,2*pi);previous=*amount;near(accumulated,angle,1e-8);}
  const auto again=g::drag_direction(l,start,i);CHECK(again);near(direction->x,again->x);near(direction->y,again->y);
}
void aligned_case(const render::ViewportCamera& camera,g::Mode mode){
  const auto l=make(mode,camera);
  true_projection(l,axes);
  for(int i=0;i<3;++i){const auto& axis=l.axes[static_cast<std::size_t>(i)];CHECK(axis.visible);
    if(mode==g::Mode::Rotate){
      bool picked=false;for(std::size_t n=0;n<g::ring_segments;++n)if(g::hit_test(l,axis.ring[n])==i){picked=true;break;}CHECK(picked);
      if(axis.edge_on){constrained_rotation(l,i);continue;}
      near(*g::drag_amount(l,axis.ring[0],axis.ring[16],i),pi/2);
      near(*g::drag_amount(l,axis.ring[16],axis.ring[0],i),-pi/2);
      near(*g::drag_amount(l,axis.ring[63],axis.ring[1],i),pi/16);
      near(*g::drag_amount(l,axis.ring[5],axis.ring[5],i),0);
      CHECK(!g::drag_amount(l,l.pivot,axis.ring[5],i));
    }else{
      CHECK(g::hit_test(l,axis.end)==i);const auto delta=axis.end_on?g::Point{0,-85}:sub(axis.end,l.pivot),start=axis.end_on?axis.end:add(l.pivot,mul(delta,.5));
      const auto positive=g::drag_amount(l,start,add(start,mul(delta,.4)),i),negative=g::drag_amount(l,start,sub(start,mul(delta,.4)),i);CHECK(positive&&negative);
      // Head-on views make the other two world directions screen-parallel. A
      // 0.4-handle drag therefore has this independent expected distance.
      if(mode==g::Mode::Move){near(*positive,l.world_length*.4);near(*negative,-l.world_length*.4);}
      else {near(*positive,std::exp2(.4));near(*negative,std::exp2(-.4));}
    }
  }
  const auto output=g::overlays(l);CHECK(output&&output.value().size()==3);
  for(const auto& draw:output.value())for(const auto& p:draw.vertices)CHECK(std::isfinite(p[0])&&std::isfinite(p[1])&&std::abs(p[0])<=16&&std::abs(p[1])<=16);
  if(mode!=g::Mode::Rotate)for(std::size_t i=0;i<3;++i)if(l.axes[i].end_on){
    near(l.axes[i].end.x,l.pivot.x);near(l.axes[i].end.y,l.pivot.y);
    for(const auto& p:output.value()[i].vertices){const g::Point pixel{(p[0]+1.)*l.width/2,(1.-p[1])*l.height/2};
      CHECK(length(sub(pixel,l.axes[i].end))<8.1);}
  }
}
void aligned_camera_sides_and_roll(){
  for(int axis=0;axis<3;++axis)for(double side:{-1.,1.}){
    auto camera=front(axis,side*10);
    for(auto mode:{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale})aligned_case(camera,mode);
    // Independent 45-degree camera roll, retaining exactly the same view axis.
    const scene::Vec3 f{-camera.eye.x/10,-camera.eye.y/10,-camera.eye.z/10},u{camera.up.x,camera.up.y,camera.up.z};
    const scene::Vec3 r{f.y*u.z-f.z*u.y,f.z*u.x-f.x*u.z,f.x*u.y-f.y*u.x};
    const double factor=std::sqrt(.5);camera.up={(u.x+r.x)*factor,(u.y+r.y)*factor,(u.z+r.z)*factor};
    for(auto mode:{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale})aligned_case(camera,mode);
  }
}
void true_edge_on_mapping(){
  const auto l=make(g::Mode::Rotate,front(2));
  for(const auto p:l.axes[0].ring)near(p.x,l.pivot.x);
  for(const auto p:l.axes[1].ring)near(p.y,l.pivot.y);
  // At the projected centre, the nearer branch is +Z. Local +X sends it
  // toward -Y (screen down); local +Y sends it toward +X (screen right).
  const auto x=g::drag_direction(l,l.pivot,0),y=g::drag_direction(l,l.pivot,1);CHECK(x&&y);
  near(x->x,0);near(x->y,1);near(y->x,1);near(y->y,0);
  near(*g::drag_amount(l,l.pivot,add(l.pivot,{0,85*pi/2}),0),pi/2);
  near(*g::drag_amount(l,l.pivot,add(l.pivot,{85*pi/2,0}),1),pi/2);
}
void near_alignment_case(double facing,g::Mode mode){
  auto camera=front(2);camera.eye={10*facing,0,10*std::sqrt(1-facing*facing)};
  const auto l=make(mode,camera);
  true_projection(l,axes);
  for(int i=0;i<3;++i){const auto& a=l.axes[static_cast<std::size_t>(i)];CHECK(a.visible);
    if(mode==g::Mode::Rotate){
      bool picked=false;for(std::size_t n=1;n<60;++n)if(g::hit_test(l,a.ring[n])==i){picked=true;break;}CHECK(picked);
      if(a.edge_on)constrained_rotation(l,i);
      else {near(*g::drag_amount(l,a.ring[5],a.ring[21],i),pi/2);near(*g::drag_amount(l,a.ring[21],a.ring[5],i),-pi/2);}
      const auto tangent=sub(a.ring[6],a.ring[5]);const auto one_pixel=mul(tangent,1/length(tangent));
      const auto tiny=g::drag_amount(l,a.ring[5],add(a.ring[5],one_pixel),i);CHECK(tiny&&std::abs(*tiny)<.2);
      if(i==0&&std::abs(facing)>.001){
        // For this camera, +Y projects upward and +Z projects against facing.
        // Preserve that independently known geometry on either side of the
        // interaction threshold; no screen ellipse is substituted.
        const auto u=sub(a.ring[0],l.pivot),v=sub(a.ring[16],l.pivot);
        CHECK((u.x*v.y-u.y*v.x)*facing<0);
        if(!a.edge_on){const auto right=g::drag_amount(l,a.ring[0],add(a.ring[0],{1,0}),i);CHECK(right&&*right*facing<0);}
      }
    }else{
      CHECK(g::hit_test(l,a.end)==i);const auto delta=a.end_on?g::Point{0,-85}:sub(a.end,l.pivot),one_pixel=mul(delta,1/length(delta));
      const auto tiny=g::drag_amount(l,a.end,add(a.end,one_pixel),i);CHECK(tiny);
      if(mode==g::Mode::Move)CHECK(*tiny>0&&*tiny<l.world_length*.2);
      else CHECK(*tiny>1&&*tiny<1.1);
    }
  }
  CHECK(g::overlays(l));
}
void near_alignment_transitions(){
  // Cross both interaction thresholds while every displayed sample remains on
  // the actual projected axis/plane, from either side of the aligned axis.
  for(double facing:{-.35,-.3,-.2001,-.1999,-.14,-.12,-.001,0.,.001,.12,.14,.1999,.2001,.3,.35})
    for(auto mode:{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale})near_alignment_case(facing,mode);
}
void aligned_large_coordinates_and_clamps(g::Mode mode){
  const auto near_view=make(mode,front(2));auto camera=front(2);
  camera.eye={1e9,-1e9,1e9+10};camera.target={1e9,-1e9,1e9};
  const auto far_view=g::layout({1e9,-1e9,1e9},axes,camera,1000,760,mode);CHECK(far_view);
  true_projection(far_view.value(),axes,{1e9,-1e9,1e9});
  for(int i=0;i<3;++i){const auto& a=near_view.axes[static_cast<std::size_t>(i)];const auto& b=far_view.value().axes[static_cast<std::size_t>(i)];
    CHECK(a.visible&&b.visible&&a.end_on==b.end_on&&a.edge_on==b.edge_on);
    near(a.end.x,b.end.x);near(a.end.y,b.end.y);
    if(mode==g::Mode::Rotate){for(std::size_t n=0;n<g::ring_segments;++n){near(a.ring[n].x,b.ring[n].x);near(a.ring[n].y,b.ring[n].y);}
      if(b.edge_on)constrained_rotation(far_view.value(),i);
      else near(*g::drag_amount(far_view.value(),b.ring[0],b.ring[16],i),pi/2);
    }
  }
  if(mode==g::Mode::Move){const g::Point delta{0,-85};
    const auto amount=g::drag_amount(near_view,near_view.pivot,add(near_view.pivot,mul(delta,1e6)),2);CHECK(amount);
    near(*amount,near_view.depth-near_view.camera.near_metres);
  }
}
void transformed_edge_plane(const std::array<scene::Vec3,3>& basis,int i){
  const auto u=basis[static_cast<std::size_t>((i+1)%3)];const double n=std::hypot(u.x,u.y,u.z);
  render::ViewportCamera camera;camera.eye={u.x/n*10,u.y/n*10,u.z/n*10};camera.target={};
  camera.up=std::abs(u.y/n)>.9?render::ViewportVec3{0,0,1}:render::ViewportVec3{0,1,0};
  const auto made=g::layout({},basis,camera,1000,760,g::Mode::Rotate);CHECK(made);
  const auto& l=made.value();const auto& a=l.axes[static_cast<std::size_t>(i)];CHECK(a.visible&&a.edge_on);
  true_projection(l,basis);constrained_rotation(l,i);CHECK(g::overlays(l));
}
void transformed_edge_planes(){
  const std::array<std::array<scene::Vec3,3>,2> bases{{{{{-2,0,0},{0,3,0},{0,0,1}}},{{{2,.25,0},{.75,1.5,.1},{.2,.6,2}}}}};
  for(const auto& basis:bases)for(int i=0;i<3;++i)transformed_edge_plane(basis,i);
}
void aligned_overlay_reuse(){
  std::vector<render::OverlayDraw> reused;CHECK(g::overlays(make(g::Mode::Rotate),reused));
  const auto* outer=reused.data();std::array<const std::array<float,2>*,3> storage{};
  for(std::size_t i=0;i<3;++i)storage[i]=reused[i].vertices.data();
  for(int axis=0;axis<3;++axis)for(double side:{-1.,1.})for(auto mode:{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale}){
    const auto l=make(mode,front(axis,side*10));CHECK(g::overlays(l,reused));CHECK(reused.size()==3&&reused.data()==outer);
    for(std::size_t i=0;i<3;++i)CHECK(reused[i].vertices.data()==storage[i]);
  }
}
}
int main(){move_and_scale();rotation();camera_zoom_and_roll();camera_affine_axes();camera_large_precision();camera_perspective_drag();transformed_rotation_planes();validation_and_bounds();overlay_reuse();
  aligned_camera_sides_and_roll();true_edge_on_mapping();near_alignment_transitions();transformed_edge_planes();aligned_overlay_reuse();
  for(auto mode:{g::Mode::Move,g::Mode::Rotate,g::Mode::Scale})aligned_large_coordinates_and_clamps(mode);return 0;}
