#include "race/render/light_output.hpp"
#include <cmath>
#include <limits>
#include <numbers>

namespace race::render {
namespace {
bool finite3(const std::array<double,3>& v) noexcept {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
bool within(double value,double low,double high) noexcept { return std::isfinite(value) && value>=low && value<=high; }
} // namespace

Result<ResolvedLighting> resolve_physical_lighting(const PhysicalLighting& p) noexcept {
  if (!finite3(p.direction_to_light)) return fail(ErrorCode::InvalidInput,"physical light direction is not finite");
  const double length=std::hypot(p.direction_to_light[0],p.direction_to_light[1],p.direction_to_light[2]);
  if (!std::isfinite(length) || length<1e-10) return fail(ErrorCode::InvalidInput,"physical light direction is degenerate");
  if (!within(p.illuminance_lux,0,physical_illuminance_max_lux)) return fail(ErrorCode::InvalidInput,"illuminance outside the declared lux bounds");
  if (!within(p.environment_cd_m2,0,physical_environment_max_cd_m2)) return fail(ErrorCode::InvalidInput,"environment luminance outside the declared bounds");
  for (const double c:p.background_cd_m2)
    if (!within(c,0,physical_background_max_cd_m2)) return fail(ErrorCode::InvalidInput,"background radiance outside the declared bounds");
  if (!within(p.exposure_stops,-physical_exposure_max_stops,physical_exposure_max_stops)) return fail(ErrorCode::InvalidInput,"exposure outside the declared stop range");
  if (!within(p.reference_luminance_cd_m2,physical_reference_min_cd_m2,physical_reference_max_cd_m2)) return fail(ErrorCode::InvalidInput,"reference luminance outside the declared bounds");
  if (p.target!=OutputTarget::DisplaySdr && p.target!=OutputTarget::DisplayScrgb) return fail(ErrorCode::Unsupported,"unknown output target");
  if (p.target==OutputTarget::DisplayScrgb) return fail(ErrorCode::Unsupported,"HDR presentation is not admitted in this checkpoint");
  ResolvedLighting r;
  for (std::size_t i=0;i<3;++i) {
    r.direction_to_light[i]=static_cast<float>(p.direction_to_light[i]/length);
    r.background_radiance[i]=static_cast<float>(p.background_cd_m2[i]);
  }
  r.irradiance_over_pi=static_cast<float>(p.illuminance_lux/std::numbers::pi);
  r.environment=static_cast<float>(p.environment_cd_m2);
  r.exposure_scale=static_cast<float>(std::exp2(p.exposure_stops)/p.reference_luminance_cd_m2);
  r.reference_luminance=static_cast<float>(p.reference_luminance_cd_m2);
  r.target=p.target;
  return r;
}

Result<void> validate_frame_lighting(LightingProfile profile,const PhysicalLighting& physical) noexcept {
  if (profile==LightingProfile::LegacyViewportV1) {
    if (!(physical==PhysicalLighting{})) return fail(ErrorCode::InvalidInput,"legacy lighting profile carries noncanonical physical fields");
    return {};
  }
  if (profile!=LightingProfile::PhysicalDiffuseV1) return fail(ErrorCode::Unsupported,"unknown lighting profile");
  auto resolved=resolve_physical_lighting(physical);if (!resolved) return resolved.error();
  return {};
}

Result<void> validate_physical_reflectance(const std::array<float,4>& tint,const std::array<float,4>& factor) noexcept {
  for (std::size_t c=0;c<3;++c) {
    if (!std::isfinite(tint[c]) || !std::isfinite(factor[c]) || tint[c]<0 || factor[c]<0)
      return fail(ErrorCode::InvalidInput,"physical reflectance inputs must be finite and nonnegative");
    if (tint[c]>1.f || factor[c]>1.f || static_cast<double>(tint[c])*factor[c]>1.0)
      return fail(ErrorCode::InvalidInput,"physical reflectance exceeds one; excess is not emission");
  }
  if (tint[3]!=1.f || factor[3]!=1.f) return fail(ErrorCode::InvalidInput,"physical reflectance is opaque only");
  return {};
}

double lambert_outgoing(double rho,double environment_cd_m2,double illuminance_lux,double cosine) noexcept {
  return rho*(environment_cd_m2+illuminance_lux*std::fmax(cosine,0.0)/std::numbers::pi);
}
double reinhard_rgb_v1(double exposed) noexcept { exposed=std::fmax(exposed,0.0); return exposed/(1.0+exposed); }
double srgb_encode(double d) noexcept {
  d=std::fmin(std::fmax(d,0.0),1.0);
  return d<=0.0031308 ? 12.92*d : 1.055*std::pow(d,1.0/2.4)-0.055;
}
double srgb_decode(double e) noexcept {
  e=std::fmin(std::fmax(e,0.0),1.0);
  return e<=0.04045 ? e/12.92 : std::pow((e+0.055)/1.055,2.4);
}
double display_sdr_encoded(double scene_linear,double exposure_scale) noexcept {
  return srgb_encode(reinhard_rgb_v1(scene_linear*exposure_scale));
}
std::uint8_t unorm8(double encoded) noexcept {
  const double v=std::fmin(std::fmax(encoded,0.0),1.0)*255.0;
  return static_cast<std::uint8_t>(std::lround(v));
}
Result<double> reference_hdr_display_nits(double exposed,double peak_nits,double paper_white_nits) noexcept {
  if (!within(peak_nits,physical_hdr_paper_white_min_nits,physical_hdr_peak_max_nits) ||
      !within(paper_white_nits,physical_hdr_paper_white_min_nits,peak_nits))
    return fail(ErrorCode::InvalidInput,"HDR reference curve requires 1 <= paper white <= peak <= 10000 nits");
  if (!std::isfinite(exposed) || exposed<0) return fail(ErrorCode::InvalidInput,"HDR reference curve input must be finite and nonnegative");
  return peak_nits*exposed/(exposed+peak_nits/paper_white_nits);
}
double scrgb_from_nits(double nits) noexcept { return nits/scrgb_nits_per_unit; }

Result<std::uint64_t> admit_output_allocations(std::span<const std::uint64_t> api_bytes,OutputTarget target) noexcept {
  if (target!=OutputTarget::DisplaySdr && target!=OutputTarget::DisplayScrgb) return fail(ErrorCode::Unsupported,"unknown output target");
  std::uint64_t total{};
  for (const auto bytes:api_bytes) {
    if (!bytes) return fail(ErrorCode::InvalidInput,"output allocation size must be nonzero");
    if (bytes>std::numeric_limits<std::uint64_t>::max()-total) return fail(ErrorCode::Overflow,"output allocation sum overflow");
    total+=bytes;
  }
  if (total>output_budget_bytes(target)) return fail(ErrorCode::Exhausted,"owned output allocations exceed the declared ceiling");
  return total;
}
} // namespace race::render
