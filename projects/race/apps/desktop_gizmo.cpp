#include "desktop_gizmo.hpp"
#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>

namespace race::desktop::gizmo {
namespace {
using V=scene::Vec3;
constexpr double pi=std::numbers::pi;
V vec(render::ViewportVec3 v) noexcept {return {v.x,v.y,v.z};}
V add(V a,V b) noexcept {return {a.x+b.x,a.y+b.y,a.z+b.z};}
V sub(V a,V b) noexcept {return {a.x-b.x,a.y-b.y,a.z-b.z};}
V mul(V a,double b) noexcept {return {a.x*b,a.y*b,a.z*b};}
double dot(V a,V b) noexcept {return a.x*b.x+a.y*b.y+a.z*b.z;}
V cross(V a,V b) noexcept {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool finite3(V p) noexcept {return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
bool finite2(Point p) noexcept {return std::isfinite(p.x)&&std::isfinite(p.y);}
bool bounded(V p) noexcept {return finite3(p)&&std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)})<=1e12;}
std::optional<V> unit(V p) noexcept {
  const double n=std::hypot(p.x,p.y,p.z);if(!std::isfinite(n)||n<1e-12)return {};
  return mul(p,1/n);
}
Point add(Point a,Point b) noexcept {return {a.x+b.x,a.y+b.y};}
Point sub(Point a,Point b) noexcept {return {a.x-b.x,a.y-b.y};}
Point mul(Point a,double b) noexcept {return {a.x*b,a.y*b};}
double dot(Point a,Point b) noexcept {return a.x*b.x+a.y*b.y;}
bool dimensions(const Layout& l) noexcept {
  return std::isfinite(l.width)&&std::isfinite(l.height)&&l.width>=1&&l.height>=1&&
      l.width<=render::viewport_max_dimension&&l.height<=render::viewport_max_dimension;
}
std::optional<Point> project(const Layout& l,V relative,bool clip_depth=true) noexcept {
  const double z=dot(relative,l.forward);
  if(!std::isfinite(z)||z<=1e-8||(clip_depth&&(z<l.camera.near_metres||z>l.camera.far_metres)))return {};
  const double scale=l.height/(2*z*l.tangent);
  Point out{l.width*.5+dot(relative,l.right)*scale,l.height*.5-dot(relative,l.up)*scale};
  if(!finite2(out))return {};return out;
}
double segment_distance2(Point p,Point a,Point b) noexcept {
  const auto d=sub(b,a);const double n=dot(d,d);
  const double t=n>1e-12?std::clamp(dot(sub(p,a),d)/n,0.,1.):0.;
  const auto delta=sub(p,add(a,mul(d,t)));return dot(delta,delta);
}
std::optional<Point> plane_coordinates(const Layout& l,const Axis& axis,Point p) noexcept {
  const auto ray=add(l.forward,add(mul(l.right,(2*p.x/l.width-1)*(l.width/l.height)*l.tangent),
      mul(l.up,(1-2*p.y/l.height)*l.tangent)));
  const double denominator=dot(ray,axis.direction);
  if(!std::isfinite(denominator)||std::abs(denominator)<1e-8)return {};
  const double distance=dot(l.pivot_relative,axis.direction)/denominator;
  if(!std::isfinite(distance)||distance<l.camera.near_metres||distance>l.camera.far_metres)return {};
  const auto radius=sub(mul(ray,distance),l.pivot_relative);
  const double uu=dot(axis.plane_u,axis.plane_u),vv=dot(axis.plane_v,axis.plane_v),uv=dot(axis.plane_u,axis.plane_v);
  const auto normal=cross(axis.plane_u,axis.plane_v);const double determinant=dot(normal,normal);
  if(!std::isfinite(determinant)||determinant<=1e-12*uu*vv)return {};
  const double ru=dot(radius,axis.plane_u),rv=dot(radius,axis.plane_v);
  const Point coordinates{(ru*vv-rv*uv)/determinant,(rv*uu-ru*uv)/determinant};
  const double length=std::hypot(coordinates.x,coordinates.y);
  if(!std::isfinite(length)||length<l.world_length*.05)return {};
  return mul(coordinates,1/length);
}
}

Result<Layout> layout(V pivot,const std::array<V,3>& world_axes,const render::ViewportCamera& camera,
    double width,double height,Mode mode,int active_axis) noexcept {
  Layout out;out.mode=mode;out.active_axis=active_axis;out.width=width;out.height=height;out.camera=camera;
  if(!dimensions(out)||!bounded(pivot)||!bounded(vec(camera.eye))||!bounded(vec(camera.target))||
      !finite3(vec(camera.up))||!std::isfinite(camera.vertical_fov_radians)||camera.vertical_fov_radians<.05||
      camera.vertical_fov_radians>3||!std::isfinite(camera.near_metres)||camera.near_metres<.0001||
      !std::isfinite(camera.far_metres)||camera.far_metres<=camera.near_metres||camera.far_metres>1e7||
      active_axis< -1||active_axis>2||(mode!=Mode::Move&&mode!=Mode::Rotate&&mode!=Mode::Scale))
    return fail(ErrorCode::InvalidInput,"Invalid gizmo projection input");
  for(const auto axis:world_axes)if(!finite3(axis))return fail(ErrorCode::InvalidInput,"Nonfinite gizmo axis");
  const auto forward=unit(sub(vec(camera.target),vec(camera.eye)));
  if(!forward)return fail(ErrorCode::InvalidInput,"Degenerate gizmo camera direction");
  const auto right=unit(cross(*forward,vec(camera.up)));
  if(!right)return fail(ErrorCode::InvalidInput,"Degenerate gizmo camera up");
  out.forward=*forward;out.right=*right;out.up=cross(*right,*forward);
  out.tangent=std::tan(camera.vertical_fov_radians*.5);out.pivot_relative=sub(pivot,vec(camera.eye));
  out.depth=dot(out.pivot_relative,out.forward);const auto screen=project(out,out.pivot_relative);
  if(!screen||screen->x<0||screen->x>width||screen->y<0||screen->y>height)return out;
  out.pivot=*screen;out.world_length=handle_pixels*2*out.depth*out.tangent/height;
  const auto to_pivot=unit(out.pivot_relative);
  for(std::size_t i=0;i<out.axes.size();++i){
    auto& axis=out.axes[i];
    if(mode!=Mode::Rotate){
      const auto direction=unit(world_axes[i]);if(!direction)continue;axis.direction=*direction;
      const auto end=project(out,add(out.pivot_relative,mul(*direction,out.world_length)),false);
      if(!end)continue;const auto delta=sub(*end,out.pivot);const double length=std::hypot(delta.x,delta.y);
      axis.visible=true;if(length<12){axis.end_on=true;axis.start=axis.end=*end;continue;}
      // Preserve the actual direction and shorten the centre gap for a small
      // projected stem. No diagonal screen axis is manufactured.
      axis.start=add(out.pivot,mul(delta,std::min(12.,length*.25)/length));axis.end=*end;continue;
    }
    // The cyclic local axes parameterize a transformed circle. One shared
    // normalization preserves their relative lengths, shear and handedness.
    const auto raw_u=world_axes[(i+1)%3],raw_v=world_axes[(i+2)%3];
    const double common=std::max(std::hypot(raw_u.x,raw_u.y,raw_u.z),std::hypot(raw_v.x,raw_v.y,raw_v.z));
    if(!std::isfinite(common)||common<1e-12)continue;
    axis.plane_u=mul(raw_u,1/common);axis.plane_v=mul(raw_v,1/common);
    const auto normal=cross(axis.plane_u,axis.plane_v);const auto direction=unit(normal);
    if(!direction||dot(normal,normal)<=1e-12*dot(axis.plane_u,axis.plane_u)*dot(axis.plane_v,axis.plane_v))continue;
    axis.direction=*direction;
    axis.visible=true;axis.edge_on=!to_pivot||std::abs(dot(*to_pivot,*direction))<.20;
    for(std::size_t j=0;j<ring_segments;++j){
      const double angle=2*pi*static_cast<double>(j)/static_cast<double>(ring_segments);
      const auto offset=mul(add(mul(axis.plane_u,std::cos(angle)),mul(axis.plane_v,std::sin(angle))),out.world_length);
      const auto point=project(out,add(out.pivot_relative,offset),false);if(!point){axis.visible=false;break;}axis.ring[j]=*point;
    }
    axis.ring[ring_segments]=axis.ring[0];
  }
  return out;
}

std::optional<Point> drag_direction(const Layout& l,Point start,int index) noexcept {
  if(!dimensions(l)||!finite2(start)||index<0||index>2)return {};
  const auto& axis=l.axes[static_cast<std::size_t>(index)];if(!axis.visible)return {};
  if(l.mode==Mode::Move||l.mode==Mode::Scale){
    if(axis.end_on)return Point{0,-1};
    const auto delta=sub(axis.end,l.pivot);const double length=std::hypot(delta.x,delta.y);
    if(!std::isfinite(length)||length<1e-8)return {};return mul(delta,1/length);
  }
  if(l.mode!=Mode::Rotate)return {};
  std::optional<Point> best;double best_distance=1e300,best_depth=1e300;
  for(std::size_t i=0;i<ring_segments;++i){
    const auto delta=sub(axis.ring[i+1],axis.ring[i]);const double squared=dot(delta,delta);
    if(!std::isfinite(squared)||squared<1e-12)continue;
    const double t=std::clamp(dot(sub(start,axis.ring[i]),delta)/squared,0.,1.);
    const auto difference=sub(start,add(axis.ring[i],mul(delta,t)));const double distance=dot(difference,difference);
    if(!std::isfinite(distance)||distance>best_distance+1e-8)continue;
    const double angle=2*pi*(static_cast<double>(i)+t)/static_cast<double>(ring_segments);
    const double depth=l.depth+l.world_length*(dot(axis.plane_u,l.forward)*std::cos(angle)+dot(axis.plane_v,l.forward)*std::sin(angle));
    if(!std::isfinite(depth))continue;
    if(!best||distance<best_distance-1e-8||(std::abs(distance-best_distance)<=1e-8&&depth<best_depth-1e-8)){
      best=mul(delta,1/std::sqrt(squared));best_distance=distance;best_depth=depth;
    }
  }
  return best;
}

std::optional<int> hit_test(const Layout& l,Point point,double tolerance) noexcept {
  if(!dimensions(l)||!finite2(point)||!std::isfinite(tolerance)||tolerance<=0||tolerance>64)return {};
  std::optional<int> best;double distance=tolerance*tolerance;
  // A short axis endpoint can lie on another axis's projected shaft. Preserve
  // both handles by preferring nearby endpoints before considering stem hits.
  if(l.mode!=Mode::Rotate){
    for(int i=0;i<3;++i){const auto& axis=l.axes[static_cast<std::size_t>(i)];if(!axis.visible)continue;
      const auto delta=sub(point,axis.end);const double candidate=dot(delta,delta);
      if(std::isfinite(candidate)&&candidate<=distance&&(!best||candidate<distance-1e-9||i==l.active_axis)){best=i;distance=candidate;}
    }
    if(best)return best;
  }
  for(int i=0;i<3;++i){const auto& axis=l.axes[static_cast<std::size_t>(i)];if(!axis.visible)continue;
    double candidate=segment_distance2(point,axis.start,axis.end);
    if(l.mode==Mode::Rotate){candidate=1e300;for(std::size_t j=0;j<ring_segments;++j)
      candidate=std::min(candidate,segment_distance2(point,axis.ring[j],axis.ring[j+1]));}
    if(std::isfinite(candidate)&&candidate<=distance&&(!best||candidate<distance-1e-9||i==l.active_axis)){
      best=i;distance=candidate;
    }
  }
  return best;
}

std::optional<double> drag_amount(const Layout& l,Point start,Point current,int index) noexcept {
  if(!dimensions(l)||index<0||index>2||!finite2(start)||!finite2(current)||!std::isfinite(l.world_length)||l.world_length<=0)return {};
  const auto& axis=l.axes[static_cast<std::size_t>(index)];if(!axis.visible)return {};
  if(l.mode==Mode::Rotate){
    if(axis.edge_on){
      const auto direction=drag_direction(l,start,index);if(!direction)return {};
      const double angle=dot(sub(current,start),*direction)/handle_pixels;
      if(!std::isfinite(angle))return {};return std::remainder(angle,2*pi);
    }
    const auto a=plane_coordinates(l,axis,start),b=plane_coordinates(l,axis,current);if(!a||!b)return {};
    const double angle=std::atan2(a->x*b->y-a->y*b->x,std::clamp(dot(*a,*b),-1.,1.));
    if(!std::isfinite(angle))return {};return angle;
  }
  const auto delta=sub(axis.end,l.pivot);const double squared=dot(delta,delta);
  if(!axis.end_on&&(!std::isfinite(squared)||squared<1e-12))return {};
  const double from=axis.end_on?0:dot(sub(start,l.pivot),delta)/squared,
      to=axis.end_on?(start.y-current.y)/handle_pixels:dot(sub(current,l.pivot),delta)/squared;
  if(!std::isfinite(from)||!std::isfinite(to))return {};
  if(l.mode==Mode::Scale)return std::exp2(std::clamp(to-from,-10.,10.));
  if(l.mode!=Mode::Move)return {};
  if(axis.end_on){
    double amount=(to-from)*l.world_length;if(!std::isfinite(amount))return {};
    const double depth_direction=dot(axis.direction,l.forward);
    if(std::abs(depth_direction)>1e-12){
      const double a=(l.camera.near_metres-l.depth)/depth_direction,b=(l.camera.far_metres-l.depth)/depth_direction;
      amount=std::clamp(amount,std::min(a,b),std::max(a,b));
    }
    return amount;
  }
  const double dz=l.world_length*dot(axis.direction,l.forward);
  const auto parameter=[&](double t)->std::optional<double>{
    const double denominator=l.depth+dz*(1-t);
    if(!std::isfinite(denominator)||std::abs(denominator)<1e-8*l.depth)return {};
    const double value=t*l.world_length*l.depth/denominator;
    const double depth=l.depth+value*dot(axis.direction,l.forward);
    if(!std::isfinite(value)||depth<l.camera.near_metres||depth>l.camera.far_metres)return {};return value;
  };
  const auto a=parameter(from),b=parameter(to);if(!a||!b||!std::isfinite(*b-*a))return {};return *b-*a;
}

Result<void> overlays(const Layout& l,std::vector<render::OverlayDraw>& out) {
  const auto reject=[&](const char* message)->Result<void>{
    out.clear();return fail(ErrorCode::InvalidInput,message);
  };
  if(!dimensions(l)||l.active_axis< -1||l.active_axis>2||
      (l.mode!=Mode::Move&&l.mode!=Mode::Rotate&&l.mode!=Mode::Scale))
    return reject("Invalid gizmo overlay layout");
  try {
    const auto count=static_cast<std::size_t>(std::count_if(l.axes.begin(),l.axes.end(),[](const Axis& axis){return axis.visible;}));
    // Resize only once: clearing/replacing the outer vector would destroy the
    // retained vertex buffers on every frame. Shrinking intentionally releases
    // hidden draw slots rather than retaining placeholders in RenderFrame.
    out.reserve(3);out.resize(count);std::size_t next{};
    constexpr std::array<std::array<float,4>,3> colors{{{.95f,.12f,.10f,1},{.13f,.82f,.22f,1},{.16f,.42f,1.f,1}}};
    for(int pass=0;pass<4;++pass){
      const int index=pass==3?l.active_axis:pass;if(index<0||index>2||(pass<3&&index==l.active_axis))continue;
      const auto& axis=l.axes[static_cast<std::size_t>(index)];if(!axis.visible)continue;
      auto& draw=out[next++];draw.color=index==l.active_axis?std::array<float,4>{1,.72f,.12f,1}:colors[static_cast<std::size_t>(index)];
      draw.vertices.clear();draw.vertices.reserve(l.mode==Mode::Rotate?ring_segments*6:96);
      bool valid=true;const auto vertex=[&](Point p){
        if(!finite2(p)){valid=false;return;}
        const double x=2*p.x/l.width-1,y=1-2*p.y/l.height;
        if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>16||std::abs(y)>16){valid=false;return;}
        draw.vertices.push_back({static_cast<float>(x),static_cast<float>(y)});
      };
      const auto triangle=[&](Point a,Point b,Point c){vertex(a);vertex(b);vertex(c);};
      const auto segment=[&](Point a,Point b){
        const auto d=sub(b,a);const double length=std::hypot(d.x,d.y);
        if(!std::isfinite(length)){valid=false;return;}
        if(length<1e-8){if(l.mode!=Mode::Rotate||!axis.edge_on)valid=false;return;}
        const Point n{-d.y/length*1.8,d.x/length*1.8};
        triangle(add(a,n),sub(a,n),add(b,n));triangle(add(b,n),sub(a,n),sub(b,n));
      };
      if(l.mode==Mode::Rotate){for(std::size_t j=0;j<ring_segments;++j)segment(axis.ring[j],axis.ring[j+1]);}
      else {if(!axis.end_on)segment(axis.start,axis.end);
        if(l.mode==Mode::Move&&axis.end_on){
          for(int i=0;i<16;++i){const double a=2*pi*i/16,b=2*pi*(i+1)/16;
            segment(add(axis.end,{6*std::cos(a),6*std::sin(a)}),add(axis.end,{6*std::cos(b),6*std::sin(b)}));}
        }else if(l.mode==Mode::Move){const auto d=sub(axis.end,axis.start);const double length=std::hypot(d.x,d.y);
          if(!std::isfinite(length)||length<1e-8)valid=false;
          else {const auto u=mul(d,1/length);const double head=std::min(12.,length*.55),half_width=std::min(5.5,head*.46);
            const Point n{-u.y*half_width,u.x*half_width};const auto base=sub(axis.end,mul(u,head));triangle(axis.end,add(base,n),sub(base,n));}
        }else {const Point a{axis.end.x-4.5,axis.end.y-4.5},b{axis.end.x+4.5,axis.end.y-4.5},c{axis.end.x+4.5,axis.end.y+4.5},d{axis.end.x-4.5,axis.end.y+4.5};triangle(a,b,c);triangle(a,c,d);}
      }
      if(!valid||draw.vertices.empty())return reject("Nonfinite or degenerate gizmo overlay geometry");
    }
    return {};
  }catch(const std::bad_alloc&){out.clear();return fail(ErrorCode::Exhausted,"Gizmo overlay allocation failed");}
}
Result<std::vector<render::OverlayDraw>> overlays(const Layout& l) {
  std::vector<render::OverlayDraw> out;auto rebuilt=overlays(l,out);
  if(!rebuilt)return rebuilt.error();return out;
}
} // namespace race::desktop::gizmo
