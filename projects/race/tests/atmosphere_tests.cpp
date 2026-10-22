#include "race/atmosphere/atmosphere.hpp"
#include "test.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string>
using namespace race;
using namespace race::atmosphere;
namespace {
bool near(double a,double b,double tolerance=1e-10) {
  return std::abs(a-b)<=tolerance*std::max({1.0,std::abs(a),std::abs(b)});
}
void finite(Spectrum s) {
  CHECK(std::isfinite(s.r)&&std::isfinite(s.g)&&std::isfinite(s.b));
  CHECK(s.r>=0&&s.g>=0&&s.b>=0);
}
Model uniform() {
  Model m;m.planet_center_m={};m.ground_radius_m=1000;m.top_radius_m=1100;
  m.molecular={{},{0.01,0.02,0.03},{DensityLaw::Uniform,1}};
  m.aerosol={{},{},{DensityLaw::Uniform,1}};
  return m;
}
void geometry() {
  const auto m=uniform();
  auto zenith=trace(m,{{0,1000,0},{0,1,0}});CHECK(zenith);
  CHECK(zenith.value().intersects_atmosphere&&!zenith.value().hit_ground);
  CHECK(near(zenith.value().entry_m,0)&&near(zenith.value().exit_m,100));
  auto nadir=trace(m,{{0,1050,0},{0,-1,0}});CHECK(nadir);
  CHECK(nadir.value().hit_ground&&near(nadir.value().exit_m,50));
  auto grounded=trace(m,{{0,1000,0},{0,-1,0}});CHECK(grounded);
  CHECK(grounded.value().hit_ground&&!grounded.value().intersects_atmosphere);
  auto horizon=trace(m,{{0,1000,0},{1,0,0}});CHECK(horizon);
  CHECK(!horizon.value().hit_ground&&near(horizon.value().exit_m,std::sqrt(210000.0)));
  auto space=trace(m,{{0,1200,0},{0,-1,0}});CHECK(space);
  CHECK(space.value().hit_ground&&near(space.value().entry_m,100)&&near(space.value().exit_m,200));
  auto escaping=trace(m,{{0,1200,0},{0,1,0}});CHECK(escaping);
  CHECK(!escaping.value().intersects_atmosphere&&!escaping.value().hit_ground);
  auto short_ray=trace(m,{{0,1050,0},{0,-1,0},25});CHECK(short_ray);
  CHECK(!short_ray.value().hit_ground&&near(short_ray.value().exit_m,25));
  auto edge=trace(m,{{0,1100,0},{0,1,0}});CHECK(edge);
  CHECK(!edge.value().intersects_atmosphere&&!edge.value().hit_ground);
  // Near-to-far root stability when atmospheric entry is far from the observer.
  auto distant=trace(m,{{0,1e10,0},{0,-1,0}});CHECK(distant);
  CHECK(near(distant.value().entry_m,1e10-1100,1e-14));
  CHECK(near(distant.value().exit_m,1e10-1000,1e-14));
  // A ray that misses ground can traverse an unobstructed negative-Y direction.
  auto negative_y=trace(m,{{0,1200,0},{1,-0.2,0}});CHECK(negative_y);
  CHECK(!negative_y.value().hit_ground);
}
void beer_lambert() {
  auto m=uniform();const Ray ray{{0,1000,0},{0,1,0}};
  auto t=transmittance(m,ray,{16,8,1000});CHECK(t);
  CHECK(near(t.value().transmittance.r,std::exp(-1.0)));
  CHECK(near(t.value().transmittance.g,std::exp(-2.0)));
  CHECK(near(t.value().transmittance.b,std::exp(-3.0)));
  CHECK(t.value().density_samples==16);
  auto first=transmittance(m,{ray.origin_m,ray.direction,40});CHECK(first);
  auto second=transmittance(m,{{0,1040,0},ray.direction,60});CHECK(second);
  CHECK(near(first.value().transmittance.r*second.value().transmittance.r,t.value().transmittance.r));
  auto reverse=transmittance(m,{{0,1100,0},{0,-1,0},100});CHECK(reverse);
  CHECK(near(reverse.value().transmittance.r,t.value().transmittance.r));
  auto scaled=transmittance(m,{ray.origin_m,{0,1e200,0}});CHECK(scaled);
  CHECK(near(scaled.value().transmittance.r,t.value().transmittance.r));
  auto tiny=transmittance(m,{ray.origin_m,{0,1e-200,0}});CHECK(tiny);
  CHECK(near(tiny.value().transmittance.r,t.value().transmittance.r));
  m.molecular.absorption_per_m={};
  auto vacuum=transmittance(m,ray);CHECK(vacuum);
  CHECK(vacuum.value().transmittance.r==1&&vacuum.value().optical_depth.r==0);
  auto empty=transmittance(m,{ray.origin_m,ray.direction,0});CHECK(empty);
  CHECK(empty.value().density_samples==0&&empty.value().transmittance.r==1);
}
void convergence() {
  auto m=uniform();m.molecular.density={DensityLaw::Exponential,30};
  const double expected=std::exp(-0.01*30*(1-std::exp(-100.0/30)));
  auto coarse=transmittance(m,{{0,1000,0},{0,1,0}},{8,8,1000});CHECK(coarse);
  auto fine=transmittance(m,{{0,1000,0},{0,1,0}},{256,8,1000});CHECK(fine);
  const double e0=std::abs(coarse.value().transmittance.r-expected);
  const double e1=std::abs(fine.value().transmittance.r-expected);
  CHECK(e1<e0/500&&e1<2e-6);
}
void scattering() {
  auto m=uniform();m.molecular.scattering_per_m={0.004,0.004,0.004};
  m.molecular.absorption_per_m={0.006,0.006,0.006};
  const Ray ray{{0,1000,0},{0,1,0}};
  const Sun sun{{0,1,0},{2,3,4}};
  auto lit=single_scattering(m,ray,sun,{16,16,1000});CHECK(lit);
  // Constant extinction, radial forward scattering: view + sun length is exactly
  // 100 m, so the full integral has this analytic solution, independent of steps.
  const double phase=3.0/(8*std::numbers::pi_v<double>);
  const double expected=2*0.004*phase*100*std::exp(-1.0);
  CHECK(near(lit.value().radiance_w_m2_sr.r,expected,1e-12));
  CHECK(near(lit.value().radiance_w_m2_sr.g,expected*1.5,1e-12));
  CHECK(lit.value().density_samples==16*17);
  auto dark=single_scattering(m,ray,{{0,-1,0},{2,3,4}});CHECK(dark);
  CHECK(dark.value().radiance_w_m2_sr.r==0&&dark.value().density_samples==64);
  auto no_sun=single_scattering(m,ray,{{0,1,0},{}});CHECK(no_sun);
  CHECK(no_sun.value().radiance_w_m2_sr.r==0);
  m.molecular.scattering_per_m={};
  auto absorbing=single_scattering(m,ray,sun);CHECK(absorbing);
  CHECK(absorbing.value().radiance_w_m2_sr.r==0);
  Model earth;
  for(Vec3 d : {Vec3{0,1,0},Vec3{1,0,0},Vec3{0,-1,0},Vec3{1,-0.1,0},Vec3{0,0,1}}) {
    for(double altitude : {2.0,10000.0,150000.0}) {
      auto result=single_scattering(earth,{{0,altitude,0},d},{});CHECK(result);
      finite(result.value().radiance_w_m2_sr);finite(result.value().transmission.transmittance);
      CHECK(result.value().transmission.transmittance.r<=1);
      CHECK(result.value().density_samples<=100000);
    }
  }
  // All directions are evaluated geometrically: no negative-Y black shortcut.
  auto below=single_scattering(earth,{{0,150000,0},{1,-0.2,0}},{});CHECK(below);
  CHECK(!below.value().transmission.interval.hit_ground);
  CHECK(below.value().radiance_w_m2_sr.r>0);
  auto base=single_scattering(earth,{{0,2,0},{1,0.05,0}},{});CHECK(base);
  const Vec3 offset{1e8,2e8,-3e8};
  earth.planet_center_m={offset.x,offset.y-6360000,offset.z};
  auto moved=single_scattering(earth,{{offset.x,offset.y+2,offset.z},{1,0.05,0}},{});CHECK(moved);
  CHECK(near(base.value().radiance_w_m2_sr.r,moved.value().radiance_w_m2_sr.r,1e-10));
}
void rejection() {
  Model m;Ray ray;
  auto invalid=m;invalid.ground_radius_m=0;CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  invalid=m;invalid.top_radius_m=m.ground_radius_m;CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  invalid=m;invalid.molecular.scattering_per_m.r=-1;CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  invalid=m;invalid.aerosol.absorption_per_m.g=std::numeric_limits<double>::quiet_NaN();CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  invalid=m;invalid.aerosol.density.scale_height_m=0;CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  invalid=m;invalid.aerosol_asymmetry=1;CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  invalid=m;invalid.molecular.density.law=static_cast<DensityLaw>(99);CHECK_ERROR(validate(invalid),ErrorCode::InvalidInput);
  ray.origin_m={0,-1,0};CHECK_ERROR(trace(m,ray),ErrorCode::InvalidInput);
  ray={};ray.direction={};CHECK_ERROR(trace(m,ray),ErrorCode::InvalidInput);
  ray={};ray.max_distance_m=-1;CHECK_ERROR(trace(m,ray),ErrorCode::InvalidInput);
  ray={};ray.direction.x=std::numeric_limits<double>::infinity();CHECK_ERROR(trace(m,ray),ErrorCode::InvalidInput);
  CHECK_ERROR(transmittance(m,{}, {0,8,100}),ErrorCode::Exhausted);
  CHECK_ERROR(transmittance(m,{}, {64,8,63}),ErrorCode::Exhausted);
  CHECK_ERROR(single_scattering(m,{}, {}, {64,32,2000}),ErrorCode::Exhausted);
  CHECK_ERROR(single_scattering(m,{}, {{0,1,0},{-1,1,1}}),ErrorCode::InvalidInput);
  CHECK_ERROR(single_scattering(m,{}, {{},{1,1,1}}),ErrorCode::InvalidInput);
}
void panorama() {
  const Budget b{8,4,100};
  auto image=panorama_ppm({}, {0,2,0},{},16,8,4,b,5120);CHECK(image);
  const std::string header="P6\n16 8\n255\n";
  CHECK(image.value().size()==header.size()+16*8*3);
  CHECK(std::equal(header.begin(),header.end(),image.value().begin()));
  CHECK(std::any_of(image.value().begin()+static_cast<std::ptrdiff_t>(header.size()),
                    image.value().end(),[](auto c){return c!=0;}));
  auto repeat=panorama_ppm({}, {0,2,0},{},16,8,4,b,5120);CHECK(repeat);
  CHECK(image.value()==repeat.value());
  CHECK_ERROR(panorama_ppm({}, {},{},16,8,4,b,5119),ErrorCode::Exhausted);
  CHECK_ERROR(panorama_ppm({}, {},{},1025,8,4,b),ErrorCode::Exhausted);
  CHECK_ERROR(panorama_ppm({}, {},{},16,8,-1,b),ErrorCode::InvalidInput);
}
} // namespace
int main() {
  geometry();beer_lambert();convergence();scattering();rejection();panorama();
  std::puts("atmosphere CPU reference: geometry, optical integration, scattering and bounds passed");
}
