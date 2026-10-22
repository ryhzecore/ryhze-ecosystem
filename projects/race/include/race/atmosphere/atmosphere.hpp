#pragma once
#include "race/foundation/result.hpp"
#include <cstdint>
#include <vector>

namespace race::atmosphere {
// Domain-owned doubles, metres, right-handed Y-up. No Scene or backend types.
struct Vec3 { double x{}, y{}, z{}; };
struct Spectrum { double r{}, g{}, b{}; };
enum class DensityLaw { Exponential, Uniform };
struct DensityProfile {
  DensityLaw law{DensityLaw::Exponential};
  double scale_height_m{8000}; // exp(-altitude/height), or exactly 1 for Uniform.
};
struct Medium {
  // Sea-level coefficients in inverse metres. Extinction = scattering + absorption.
  Spectrum scattering_per_m{}, absorption_per_m{};
  DensityProfile density{};
};
struct Model {
  // Earth-sized illustrative profile; local ground below observer (0,0,0).
  Vec3 planet_center_m{0, -6360000, 0};
  double ground_radius_m{6360000}, top_radius_m{6460000};
  Medium molecular{{5.802e-6, 13.558e-6, 33.1e-6}, {}, {DensityLaw::Exponential, 8000}};
  Medium aerosol{{3.996e-6, 3.996e-6, 3.996e-6}, {0.444e-6, 0.444e-6, 0.444e-6},
                 {DensityLaw::Exponential, 1200}};
  double aerosol_asymmetry{0.8}; // Cornette-Shanks g, admitted [-0.95,0.95].
};
struct Ray {
  Vec3 origin_m{}, direction{0, 1, 0}; // Nonzero directions are normalized internally.
  double max_distance_m{1.0e12};       // Finite endpoint; zero is a valid empty ray.
};
struct Sun {
  Vec3 direction_to_sun{0, 1, 0};
  // Irradiance normal to the incoming beam, W/m^2 in three declared linear bands.
  // This directional-light reference does not resolve the finite solar disc.
  Spectrum irradiance_w_m2{1.5, 1.5, 1.5};
};
struct Budget {
  std::uint32_t view_steps{64}, sun_steps{32};
  std::uint64_t max_density_samples{100000};
};
struct RayInterval {
  double entry_m{}, exit_m{}; // From original ray origin, clipped at ground/end.
  bool intersects_atmosphere{}, hit_ground{};
};
struct Transmission {
  Spectrum transmittance{1,1,1}, optical_depth{};
  RayInterval interval;
  std::uint64_t density_samples{};
};
struct Scattering {
  Spectrum radiance_w_m2_sr{};
  Transmission transmission;
  std::uint64_t density_samples{}; // View plus all sun-path density evaluations.
};
// All per-ray functions are allocation-free and return errors before integration
// if input or worst-case requested work exceeds its declared bounds.
Result<void> validate(const Model &) noexcept;
Result<RayInterval> trace(const Model &, const Ray &) noexcept;
// Attenuation is for the medium segment only. hit_ground separately reports the
// opaque boundary: do not multiply a background sky by transmission through ground.
Result<Transmission> transmittance(const Model &, const Ray &, Budget = {}) noexcept;
Result<Scattering> single_scattering(const Model &, const Ray &, const Sun &, Budget = {}) noexcept;
// Full-sphere equirectangular diagnostic, +Y at zenith, azimuth zero towards +Z.
// P6 PPM bytes, bounded to 1024 per side/262144 pixels/100 million density samples.
// Preview applies 1-exp(-exposure*L) then sRGB encoding; it is not HDR/film output.
// Ground is opaque and unlit here. No ground bounce, stars, clouds or emission.
Result<std::vector<std::uint8_t>> panorama_ppm(const Model &, Vec3 observer_m,
    const Sun &, std::uint32_t width, std::uint32_t height, double exposure,
    Budget = {}, std::uint64_t total_sample_budget = 100000000);
} // namespace race::atmosphere
