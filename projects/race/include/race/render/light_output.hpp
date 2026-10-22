#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace race::render {
// P03.04 physical lighting and declared output (docs/progress/p03-light-output-contracts-20261013.md).
// Value-only: no OS handles, Scene pointers or native formats. LegacyViewportV1 keeps the
// existing unitless RenderFrame multipliers byte-compatible; PhysicalDiffuseV1 is an explicit
// opt-in carrying SI quantities. A frame has exactly one profile; the models are never combined.
enum class LightingProfile : std::uint8_t { LegacyViewportV1=1, PhysicalDiffuseV1=2 };
// scRGB is declared so requests can be rejected explicitly; HDR presentation is not admitted
// in this checkpoint (contract: an explicit HDR request rejects with Unsupported).
enum class OutputTarget : std::uint8_t { DisplaySdr=1, DisplayScrgb=2 };
enum class ToneTransform : std::uint8_t { ReinhardRgbV1=1 };

// Fixed working space of PhysicalDiffuseV1: scene-linear Rec.709/sRGB primaries, D65 white,
// luminance scale (1,1,1) == 1 cd/m^2. RGB is a tristimulus approximation, not a spectrum.
inline constexpr std::string_view physical_working_space_id="linear-rec709-d65-cd-m2";
inline constexpr std::string_view physical_tone_transform_id="ReinhardRGB-v1";
inline constexpr std::string_view physical_hdr_reference_curve_id="BoundedPeakPaperWhite-v1";

// Declared finite bounds of the first physical profile (profile choices, not physical limits).
inline constexpr double physical_illuminance_max_lux=100000.0;
inline constexpr double physical_environment_max_cd_m2=1000.0;
inline constexpr double physical_background_max_cd_m2=60000.0;
inline constexpr double physical_exposure_max_stops=20.0;
inline constexpr double physical_reference_min_cd_m2=1.0, physical_reference_max_cd_m2=10000.0;
inline constexpr double physical_hdr_paper_white_min_nits=1.0, physical_hdr_peak_max_nits=10000.0;
inline constexpr double scrgb_nits_per_unit=80.0;
// Separately declared API-allocation ceilings for the owned output set (owner decision (c)).
inline constexpr std::uint64_t output_budget_physical_sdr_bytes=520ull*1024*1024;
inline constexpr std::uint64_t output_budget_physical_scrgb_bytes=712ull*1024*1024;

struct PhysicalLighting {
  std::array<double,3> direction_to_light{0,1,0}; // World direction toward the light; normalized on admission.
  double illuminance_lux{};                        // Normal-incidence illuminance E of the white D65 directional light.
  double environment_cd_m2{};                      // Uniform achromatic environment luminance.
  std::array<double,3> background_cd_m2{};         // Physical background radiance, scene-linear RGB.
  double exposure_stops{};                         // Applied only at the output boundary; never alters stored HDR.
  double reference_luminance_cd_m2{100.0};         // Scene value mapped to exposed 1.0 before the tone transform.
  OutputTarget target{OutputTarget::DisplaySdr};
  friend bool operator==(const PhysicalLighting&,const PhysicalLighting&)=default;
};
// Adapter-facing values for one frame. `irradiance_over_pi` is E/pi so the shader evaluates
// rho*(environment + irradiance_over_pi*max(dot(n,l),0)); `exposure_scale` is 2^stops/R.
struct ResolvedLighting {
  LightingProfile profile{LightingProfile::PhysicalDiffuseV1};
  std::array<float,3> direction_to_light{};
  float irradiance_over_pi{},environment{},exposure_scale{},reference_luminance{};
  std::array<float,3> background_radiance{};
  OutputTarget target{OutputTarget::DisplaySdr};
  ToneTransform tone{ToneTransform::ReinhardRgbV1};
};
// Rejects NaN/infinity, zero or degenerate directions, out-of-range quantities, unknown enums
// and scRGB presentation before any allocation or submission.
Result<ResolvedLighting> resolve_physical_lighting(const PhysicalLighting&) noexcept;
// Legacy frames must carry default (inactive) physical fields; physical frames must resolve.
Result<void> validate_frame_lighting(LightingProfile,const PhysicalLighting&) noexcept;
// PhysicalDiffuseV1 reflectance rho = factor * tint (* texture in [0,1]); each channel in [0,1].
// Excess reflectance is rejected, never clamped or reinterpreted as emission.
Result<void> validate_physical_reflectance(const std::array<float,4>& tint,const std::array<float,4>& material_factor) noexcept;

// Reference equations in double precision; the GPU path must agree within declared tolerances.
double lambert_outgoing(double rho,double environment_cd_m2,double illuminance_lux,double cosine) noexcept;
double reinhard_rgb_v1(double exposed) noexcept;          // y = x / (1 + x)
double srgb_encode(double display_linear) noexcept;       // piecewise IEC 61966-2-1, applied once at the output boundary
double srgb_decode(double encoded) noexcept;
double display_sdr_encoded(double scene_linear,double exposure_scale) noexcept; // srgb_encode(reinhard(x*scale)) in [0,1]
std::uint8_t unorm8(double encoded) noexcept;             // round to nearest, clamped
// Bounded reference HDR curve: display_nits = peak*x/(x+peak/paper_white); approaches peak and
// has low-level slope paper_white. Not a standardized mastering curve; numerically tested only.
Result<double> reference_hdr_display_nits(double exposed,double peak_nits,double paper_white_nits) noexcept;
double scrgb_from_nits(double nits) noexcept;              // scRGB 1.0 == 80 nits

// Owned output set budget: checked 64-bit sum of API-reported allocation sizes against the
// declared ceiling of the requested target. Over budget is Exhausted, never a silent reduction.
Result<std::uint64_t> admit_output_allocations(std::span<const std::uint64_t> api_bytes,OutputTarget) noexcept;
constexpr std::uint64_t output_budget_bytes(OutputTarget target) noexcept {
  return target==OutputTarget::DisplayScrgb ? output_budget_physical_scrgb_bytes : output_budget_physical_sdr_bytes;
}
} // namespace race::render
