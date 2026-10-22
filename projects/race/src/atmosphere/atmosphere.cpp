#include "race/atmosphere/atmosphere.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <string>

namespace race::atmosphere {
namespace {
constexpr double pi = std::numbers::pi_v<double>;
constexpr std::uint32_t max_steps = 1024;
constexpr std::uint64_t max_samples = 2'000'000;
double dot(Vec3 a, Vec3 b) noexcept { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 add(Vec3 a, Vec3 b) noexcept { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) noexcept { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 mul(Vec3 a, double b) noexcept { return {a.x*b,a.y*b,a.z*b}; }
Vec3 cross(Vec3 a, Vec3 b) noexcept {
  return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
double length(Vec3 a) noexcept { return std::hypot(a.x,a.y,a.z); }
bool bounded(double x, double lo, double hi) noexcept {
  return std::isfinite(x) && x>=lo && x<=hi;
}
bool position(Vec3 p) noexcept {
  return bounded(p.x,-1e12,1e12)&&bounded(p.y,-1e12,1e12)&&bounded(p.z,-1e12,1e12);
}
Result<Vec3> direction(Vec3 p) noexcept {
  if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))
    return fail(ErrorCode::InvalidInput,"Atmosphere direction must be finite");
  // Scale before normalizing, admitting finite very small/large input vectors.
  const double scale=std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)});
  if(scale==0) return fail(ErrorCode::InvalidInput,"Atmosphere direction cannot be zero");
  p={p.x/scale,p.y/scale,p.z/scale};
  return mul(p,1.0/length(p));
}
bool spectrum(Spectrum x,double maximum) noexcept {
  return bounded(x.r,0,maximum)&&bounded(x.g,0,maximum)&&bounded(x.b,0,maximum);
}
Spectrum plus(Spectrum a,Spectrum b) noexcept { return {a.r+b.r,a.g+b.g,a.b+b.b}; }
Spectrum times(Spectrum a,double b) noexcept { return {a.r*b,a.g*b,a.b*b}; }
Spectrum product(Spectrum a,Spectrum b) noexcept { return {a.r*b.r,a.g*b.g,a.b*b.b}; }
Spectrum exp_negative(Spectrum a) noexcept { return {std::exp(-a.r),std::exp(-a.g),std::exp(-a.b)}; }
bool medium(const Medium &m) noexcept {
  return spectrum(m.scattering_per_m,1)&&spectrum(m.absorption_per_m,1)&&
    (m.density.law==DensityLaw::Uniform||m.density.law==DensityLaw::Exponential)&&
    bounded(m.density.scale_height_m,0.1,1e8);
}
Result<void> budget_valid(Budget b) noexcept {
  if(b.view_steps==0||b.sun_steps==0||b.view_steps>max_steps||b.sun_steps>max_steps||
     b.max_density_samples==0||b.max_density_samples>max_samples)
    return fail(ErrorCode::Exhausted,"Atmosphere integration budget outside supported bounds");
  return {};
}
Result<Vec3> ray_valid(const Model &m,const Ray &r) noexcept {
  if(!position(r.origin_m)||!bounded(r.max_distance_m,0,4e12))
    return fail(ErrorCode::InvalidInput,"Atmosphere ray position or extent outside bounds");
  if(length(sub(r.origin_m,m.planet_center_m))<m.ground_radius_m)
    return fail(ErrorCode::InvalidInput,"Atmosphere observer is inside the opaque planet");
  return direction(r.direction);
}
// Stable quadratic roots. The cross-product discriminant avoids subtracting two
// almost equal observer-distance squares on rays arriving from distant space.
struct Roots { double near{}, far{}; bool intersects{}; };
Roots sphere_roots(Vec3 o,Vec3 d,double radius) noexcept {
  const double b=dot(o,d),r=length(o);
  const Vec3 perpendicular=cross(o,d);
  const double disc=radius*radius-dot(perpendicular,perpendicular);
  if(disc<0) return {};
  const double root=std::sqrt(disc);
  const double q=-b-std::copysign(root,b);
  if(q==0) return {0,0,true};
  const double c=(r-radius)*(r+radius);
  const double other=c/q;
  return {std::min(q,other),std::max(q,other),true};
}
RayInterval interval(const Model &m,Vec3 local,Vec3 d,double maximum) noexcept {
  RayInterval result;
  const auto outer=sphere_roots(local,d,m.top_radius_m);
  if(!outer.intersects||outer.far<0||maximum==0) return result;
  const double entry=std::max(0.0,outer.near);
  double exit=std::min(maximum,outer.far);
  if(exit<entry) return result;
  const auto ground=sphere_roots(local,d,m.ground_radius_m);
  // A tangent merely touches the mathematical surface; it does not enter ground.
  if(ground.intersects&&ground.far>ground.near&&ground.far>0&&
     ground.near>=0&&ground.near<=exit) {
    exit=ground.near;result.hit_ground=true;
  }
  result.entry_m=entry;result.exit_m=std::max(entry,exit);
  result.intersects_atmosphere=result.exit_m>entry;
  return result;
}
double density(const DensityProfile &p,double altitude) noexcept {
  return p.law==DensityLaw::Uniform?1:std::exp(-std::max(0.0,altitude)/p.scale_height_m);
}
struct OpticalSample { Spectrum extinction,molecular,aerosol; };
OpticalSample sample(const Model &m,Vec3 point) noexcept {
  const double h=length(point)-m.ground_radius_m;
  const double dm=density(m.molecular.density,h),da=density(m.aerosol.density,h);
  return {plus(times(plus(m.molecular.scattering_per_m,m.molecular.absorption_per_m),dm),
               times(plus(m.aerosol.scattering_per_m,m.aerosol.absorption_per_m),da)),
          times(m.molecular.scattering_per_m,dm),times(m.aerosol.scattering_per_m,da)};
}
Spectrum depth(const Model &m,Vec3 origin,Vec3 d,RayInterval path,std::uint32_t steps) noexcept {
  Spectrum sum;
  if(!path.intersects_atmosphere) return sum;
  const double ds=(path.exit_m-path.entry_m)/steps;
  for(std::uint32_t i=0;i<steps;++i) {
    const double distance=path.entry_m+(static_cast<double>(i)+0.5)*ds;
    sum=plus(sum,times(sample(m,add(origin,mul(d,distance))).extinction,ds));
  }
  return sum;
}
double rayleigh_phase(double cosine) noexcept { return 3*(1+cosine*cosine)/(16*pi); }
double aerosol_phase(double cosine,double g) noexcept {
  const double denominator=1+g*g-2*g*cosine;
  return (3/(8*pi))*(1-g*g)*(1+cosine*cosine)/
    ((2+g*g)*denominator*std::sqrt(denominator));
}
Scattering integrate(const Model &m,Vec3 o,Vec3 d,const Sun &sun,Vec3 sun_direction,
                     Budget b,RayInterval path) noexcept {
  Scattering result;result.transmission.interval=path;
  if(!path.intersects_atmosphere) return result;
  const double ds=(path.exit_m-path.entry_m)/b.view_steps;
  const double cosine=std::clamp(dot(d,sun_direction),-1.0,1.0);
  const double pr=rayleigh_phase(cosine),pa=aerosol_phase(cosine,m.aerosol_asymmetry);
  Spectrum view_depth;
  for(std::uint32_t i=0;i<b.view_steps;++i) {
    const double distance=path.entry_m+(static_cast<double>(i)+0.5)*ds;
    const auto point=add(o,mul(d,distance));
    const auto optical=sample(m,point);
    ++result.density_samples;
    const auto increment=times(optical.extinction,ds);
    const auto light_path=interval(m,point,sun_direction,4e12);
    if(!light_path.hit_ground) {
      const auto solar_depth=depth(m,point,sun_direction,light_path,b.sun_steps);
      if(light_path.intersects_atmosphere) result.density_samples+=b.sun_steps;
      const auto attenuation=exp_negative(plus(plus(view_depth,times(increment,0.5)),solar_depth));
      const auto scattering=plus(times(optical.molecular,pr),times(optical.aerosol,pa));
      result.radiance_w_m2_sr=plus(result.radiance_w_m2_sr,
          times(product(product(sun.irradiance_w_m2,attenuation),scattering),ds));
    }
    view_depth=plus(view_depth,increment);
  }
  result.transmission.optical_depth=view_depth;
  result.transmission.transmittance=exp_negative(view_depth);
  result.transmission.density_samples=b.view_steps;
  return result;
}
std::uint8_t preview(double radiance,double exposure) noexcept {
  const double mapped=-std::expm1(-radiance*exposure);
  const double srgb=mapped<=0.0031308?12.92*mapped:1.055*std::pow(mapped,1/2.4)-0.055;
  return static_cast<std::uint8_t>(std::lround(std::clamp(srgb,0.0,1.0)*255));
}
} // namespace
Result<void> validate(const Model &m) noexcept {
  if(!position(m.planet_center_m)||!bounded(m.ground_radius_m,1,1e9)||
     !bounded(m.top_radius_m,m.ground_radius_m+1,m.ground_radius_m+1e8)||
     !medium(m.molecular)||!medium(m.aerosol)||!bounded(m.aerosol_asymmetry,-0.95,0.95))
    return fail(ErrorCode::InvalidInput,"Atmosphere model units, coefficients or bounds are invalid");
  return {};
}
Result<RayInterval> trace(const Model &m,const Ray &ray) noexcept {
  if(auto v=validate(m);!v) return v.error();
  auto d=ray_valid(m,ray);if(!d) return d.error();
  return interval(m,sub(ray.origin_m,m.planet_center_m),d.value(),ray.max_distance_m);
}
Result<Transmission> transmittance(const Model &m,const Ray &ray,Budget b) noexcept {
  if(auto v=validate(m);!v) return v.error();
  if(auto v=budget_valid(b);!v) return v.error();
  auto d=ray_valid(m,ray);if(!d) return d.error();
  if(b.view_steps>b.max_density_samples)
    return fail(ErrorCode::Exhausted,"Atmosphere transmittance exceeds density-sample budget");
  Transmission result;
  const auto o=sub(ray.origin_m,m.planet_center_m);
  result.interval=interval(m,o,d.value(),ray.max_distance_m);
  result.optical_depth=depth(m,o,d.value(),result.interval,b.view_steps);
  result.transmittance=exp_negative(result.optical_depth);
  result.density_samples=result.interval.intersects_atmosphere?b.view_steps:0;
  return result;
}
Result<Scattering> single_scattering(const Model &m,const Ray &ray,const Sun &sun,Budget b) noexcept {
  if(auto v=validate(m);!v) return v.error();
  if(auto v=budget_valid(b);!v) return v.error();
  auto d=ray_valid(m,ray);if(!d) return d.error();
  auto sd=direction(sun.direction_to_sun);if(!sd) return sd.error();
  if(!spectrum(sun.irradiance_w_m2,1e6))
    return fail(ErrorCode::InvalidInput,"Atmosphere solar irradiance is invalid");
  if(static_cast<std::uint64_t>(b.view_steps)*(b.sun_steps+1)>b.max_density_samples)
    return fail(ErrorCode::Exhausted,"Atmosphere scattering exceeds density-sample budget");
  const auto o=sub(ray.origin_m,m.planet_center_m);
  return integrate(m,o,d.value(),sun,sd.value(),b,interval(m,o,d.value(),ray.max_distance_m));
}
Result<std::vector<std::uint8_t>> panorama_ppm(const Model &m,Vec3 observer,const Sun &sun,
    std::uint32_t width,std::uint32_t height,double exposure,Budget b,std::uint64_t total_budget) {
  if(auto v=validate(m);!v) return v.error();
  if(auto v=budget_valid(b);!v) return v.error();
  if(auto v=ray_valid(m,Ray{observer});!v) return v.error();
  auto sd=direction(sun.direction_to_sun);if(!sd) return sd.error();
  if(!spectrum(sun.irradiance_w_m2,1e6)||!bounded(exposure,0,1e6))
    return fail(ErrorCode::InvalidInput,"Atmosphere panorama solar irradiance or exposure is invalid");
  const std::uint64_t pixels=static_cast<std::uint64_t>(width)*height;
  const std::uint64_t per_ray=static_cast<std::uint64_t>(b.view_steps)*(b.sun_steps+1);
  if(width<2||height<2||width>1024||height>1024||pixels>262144||
     per_ray>b.max_density_samples||total_budget==0||total_budget>100000000||
     pixels>total_budget/per_ray)
    return fail(ErrorCode::Exhausted,"Atmosphere panorama dimensions or work budget exceeded");
  try {
    const auto header="P6\n"+std::to_string(width)+" "+std::to_string(height)+"\n255\n";
    std::vector<std::uint8_t> output(header.begin(),header.end());
    output.reserve(header.size()+static_cast<std::size_t>(pixels)*3);
    const auto o=sub(observer,m.planet_center_m);
    for(std::uint32_t y=0;y<height;++y) {
      const double elevation=pi*(0.5-(static_cast<double>(y)+0.5)/height);
      for(std::uint32_t x=0;x<width;++x) {
        const double azimuth=2*pi*((static_cast<double>(x)+0.5)/width-0.5);
        const Vec3 d{std::cos(elevation)*std::sin(azimuth),std::sin(elevation),
                     std::cos(elevation)*std::cos(azimuth)};
        const auto result=integrate(m,o,d,sun,sd.value(),b,interval(m,o,d,1e12));
        const auto radiance=result.radiance_w_m2_sr;
        output.push_back(preview(radiance.r,exposure));
        output.push_back(preview(radiance.g,exposure));
        output.push_back(preview(radiance.b,exposure));
      }
    }
    return output;
  } catch(const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted,"Atmosphere panorama allocation failed");
  }
}
} // namespace race::atmosphere
