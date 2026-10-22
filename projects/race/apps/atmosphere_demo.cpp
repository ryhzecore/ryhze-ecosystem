#include "race/atmosphere/atmosphere.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
using namespace race::atmosphere;
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t width=192,height=96;
constexpr Budget budget{32,16,100000};
constexpr std::uint64_t pixels=static_cast<std::uint64_t>(width)*height;
constexpr std::uint64_t sample_bound=pixels*budget.view_steps*(budget.sun_steps+1);
struct View { const char* name; Vec3 observer; Sun sun; double exposure; };
struct Receipt {
  std::uint64_t density_samples{},view_samples{},ground_rays{},nonblack_pixels{};
  double maximum_radiance{},integration_ms{};
  std::uintmax_t file_bytes{};
};
bool finite_nonnegative(Spectrum v) {
  return std::isfinite(v.r)&&std::isfinite(v.g)&&std::isfinite(v.b)&&v.r>=0&&v.g>=0&&v.b>=0;
}
unsigned char display(double radiance,double exposure) {
  const double mapped=-std::expm1(-radiance*exposure);
  const double encoded=mapped<=0.0031308?12.92*mapped:1.055*std::pow(mapped,1/2.4)-0.055;
  return static_cast<unsigned char>(std::lround(std::clamp(encoded,0.0,1.0)*255));
}
Receipt render(const std::filesystem::path& output,const View& view) {
  const Model model;
  Receipt receipt;
  std::vector<unsigned char> image(static_cast<std::size_t>(pixels)*3);
  const auto start=Clock::now();
  // Explicit per-pixel calls expose actual density work in the receipt. These are
  // the same equirectangular directions and preview transform as panorama_ppm.
  for(std::uint32_t y=0;y<height;++y) {
    const double elevation=std::numbers::pi_v<double>*(0.5-(static_cast<double>(y)+0.5)/height);
    for(std::uint32_t x=0;x<width;++x) {
      const double azimuth=2*std::numbers::pi_v<double>*((static_cast<double>(x)+0.5)/width-0.5);
      const Vec3 direction{std::cos(elevation)*std::sin(azimuth),std::sin(elevation),
                           std::cos(elevation)*std::cos(azimuth)};
      auto result=single_scattering(model,{view.observer,direction},view.sun,budget);
      if(!result)throw std::runtime_error(result.error().message);
      const auto& r=result.value();
      const auto t=r.transmission.transmittance;
      if(!finite_nonnegative(r.radiance_w_m2_sr)||!finite_nonnegative(t)||
         t.r>1||t.g>1||t.b>1||r.density_samples>budget.max_density_samples)
        throw std::runtime_error("nonfinite or out-of-contract atmosphere result");
      receipt.density_samples+=r.density_samples;
      receipt.view_samples+=r.transmission.density_samples;
      receipt.ground_rays+=r.transmission.interval.hit_ground?1:0;
      receipt.maximum_radiance=std::max({receipt.maximum_radiance,r.radiance_w_m2_sr.r,
                                       r.radiance_w_m2_sr.g,r.radiance_w_m2_sr.b});
      const auto index=(static_cast<std::size_t>(y)*width+x)*3;
      image[index]=display(r.radiance_w_m2_sr.r,view.exposure);
      image[index+1]=display(r.radiance_w_m2_sr.g,view.exposure);
      image[index+2]=display(r.radiance_w_m2_sr.b,view.exposure);
      receipt.nonblack_pixels+=(image[index]||image[index+1]||image[index+2])?1:0;
    }
  }
  receipt.integration_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  if(!std::isfinite(receipt.integration_ms)||receipt.density_samples>sample_bound)
    throw std::runtime_error("atmosphere work or timing bound failed");
  const auto path=output/(std::string(view.name)+".ppm");
  const auto header="P6\n"+std::to_string(width)+" "+std::to_string(height)+"\n255\n";
  std::ofstream file(path,std::ios::binary|std::ios::trunc);
  file.write(header.data(),static_cast<std::streamsize>(header.size()));
  file.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size()));
  file.close();
  if(!file)throw std::runtime_error("atmosphere PPM write failed");
  receipt.file_bytes=std::filesystem::file_size(path);
  if(receipt.file_bytes!=header.size()+image.size())throw std::runtime_error("atmosphere PPM length mismatch");
  // Verify the on-disk payload, not merely the in-memory image or stream state.
  std::ifstream readback(path,std::ios::binary);
  std::vector<unsigned char> actual(static_cast<std::size_t>(receipt.file_bytes));
  readback.read(reinterpret_cast<char*>(actual.data()),static_cast<std::streamsize>(actual.size()));
  if(!readback||!std::equal(header.begin(),header.end(),actual.begin())||
     !std::equal(image.begin(),image.end(),actual.begin()+static_cast<std::ptrdiff_t>(header.size())))
    throw std::runtime_error("atmosphere PPM readback mismatch");
  return receipt;
}
}
int main(int argc,char** argv) {
  if(argc!=2){std::cerr<<"usage: race_atmosphere_demo <output-directory>\n";return 2;}
  try {
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    const std::array<View,4> views{{
      {"day",{0,2,0},{{0,0.65,1},{1.5,1.5,1.5}},5},
      {"twilight",{0,2,0},{{0,-0.04,1},{1.5,1.5,1.5}},30},
      {"night",{0,2,0},{{0,-1,0},{1.5,1.5,1.5}},5},
      {"high-altitude",{0,150000,0},{{0,0.2,1},{1.5,1.5,1.5}},10}
    }};
    std::array<Receipt,4> receipts;
    for(std::size_t i=0;i<views.size();++i)receipts[i]=render(output,views[i]);
    std::ofstream report(output/"receipt.json",std::ios::trunc);
    report.imbue(std::locale::classic());report<<std::setprecision(17);
    report<<"{\n  \"schema\": 1,\n  \"kind\": \"CPU single-scattering diagnostic; not production sky\",\n"
          <<"  \"ground\": \"Opaque planet intersections are evaluated in all directions. Ground lighting, reflection, bounce and composition are omitted; resulting black ground is not a shipping lower-hemisphere policy.\",\n"
          <<"  \"night\": \"No stars, moon, airglow or multiple scattering; deep-night black is expected for this reference.\",\n"
          <<"  \"projection\": \"equirectangular, +Y zenith, azimuth zero +Z\",\n"
          <<"  \"display\": \"1-exp(-exposure*radiance), then sRGB\",\n"
          <<"  \"width\": "<<width<<", \"height\": "<<height<<",\n"
          <<"  \"view_steps\": "<<budget.view_steps<<", \"sun_steps\": "<<budget.sun_steps<<",\n"
          <<"  \"density_sample_bound_per_image\": "<<sample_bound<<",\n  \"images\": [\n";
    for(std::size_t i=0;i<views.size();++i) {
      const auto& v=views[i];const auto& r=receipts[i];
      report<<"    {\"file\": \""<<v.name<<".ppm\", \"observer_m\": ["<<v.observer.x<<','<<v.observer.y<<','<<v.observer.z
            <<"], \"sun_direction\": ["<<v.sun.direction_to_sun.x<<','<<v.sun.direction_to_sun.y<<','<<v.sun.direction_to_sun.z
            <<"], \"exposure\": "<<v.exposure<<", \"integration_ms\": "<<r.integration_ms
            <<", \"density_samples\": "<<r.density_samples<<", \"view_density_samples\": "<<r.view_samples
            <<", \"ground_intersection_rays\": "<<r.ground_rays<<", \"nonblack_pixels\": "<<r.nonblack_pixels
            <<", \"maximum_radiance_w_m2_sr\": "<<r.maximum_radiance<<", \"file_bytes\": "<<r.file_bytes<<'}'
            <<(i+1==views.size()?"\n":",\n");
    }
    report<<"  ]\n}\n";report.close();
    if(!report)throw std::runtime_error("atmosphere receipt write failed");
    std::cout<<"Wrote four 192x96 CPU diagnostic PPMs and receipt.json to "<<output.string()
             <<"\nGround lighting is omitted; these are not production sky/reflection images.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"Atmosphere demo failed: "<<e.what()<<'\n';return 1;}
}
