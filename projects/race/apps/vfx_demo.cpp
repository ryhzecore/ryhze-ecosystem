#include "race/vfx/vfx.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
using namespace race::vfx;
namespace {
constexpr int width=640,height=360;
constexpr double pixels_per_metre=27.0;
constexpr int ground_y=220;
bool frame(const std::filesystem::path& file,const ParticleSystem& system) {
  std::vector<unsigned char> image(static_cast<std::size_t>(width*height*3));
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    auto offset=static_cast<std::size_t>((y*width+x)*3);
    image[offset]=8;image[offset+1]=12;image[offset+2]=22;
    if(y==ground_y)image[offset]=image[offset+1]=image[offset+2]=55;
  }
  // Offline CPU projection; this image writer is not a runtime/GPU backend.
  for(const auto& p:system.particles()) {
    const double world_x=p.position.x+system.origin().x;
    const double world_y=p.position.y+system.origin().y;
    const int cx=static_cast<int>(std::lround(width*0.5+world_x*pixels_per_metre));
    const int cy=static_cast<int>(std::lround(ground_y-world_y*pixels_per_metre));
    const int radius=std::max(1,static_cast<int>(std::ceil(p.radius*pixels_per_metre)));
    const double fade=1.0-static_cast<double>(p.age_ticks)/p.lifetime_ticks;
    for(int y=std::max(0,cy-radius);y<=std::min(height-1,cy+radius);++y)
      for(int x=std::max(0,cx-radius);x<=std::min(width-1,cx+radius);++x) {
        const double distance=static_cast<double>((x-cx)*(x-cx)+(y-cy)*(y-cy));
        if(distance>radius*radius)continue;
        const double alpha=p.color[3]*fade*(1.0-0.35*distance/(radius*radius));
        for(int channel=0;channel<3;++channel) {
          auto& pixel=image[static_cast<std::size_t>((y*width+x)*3+channel)];
          pixel=static_cast<unsigned char>(std::clamp(static_cast<double>(pixel)+p.color[static_cast<std::size_t>(channel)]*alpha*255,0.0,255.0));
        }
      }
  }
  std::ofstream out(file,std::ios::binary);out<<"P6\n"<<width<<' '<<height<<"\n255\n";
  out.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size()));
  return static_cast<bool>(out);
}
}
int main(int argc,char** argv) {
  if(argc!=2){std::cerr<<"usage: race_vfx_demo <output-directory>\n";return 2;}
  try {
    const std::filesystem::path output(argv[1]);std::filesystem::create_directories(output);
    Config config;config.max_particles=1600;config.max_spawn_attempts_per_tick=128;
    auto created=ParticleSystem::create(config);if(!created)return 3;
    auto simulation=std::move(created).value();
    EmitterDesc fountain;fountain.source={42,1};fountain.seed=91237;
    fountain.position={-2,0,0};fountain.velocity={0,5.8,0};fountain.velocity_spread={1.1,1.5,0.2};
    fountain.acceleration={0,-4.2,0};fountain.particles_per_tick=8;
    fountain.lifetime_ticks=130;fountain.radius=0.035;fountain.color={0.1f,0.65f,1,0.8f};
    auto continuous=simulation->create_emitter(fountain);if(!continuous)return 4;
    auto sparks=fountain;sparks.source={42,2};sparks.position={};sparks.particles_per_tick=0;
    sparks.velocity={0,1.5,0};sparks.velocity_spread={3.0,3.0,0.4};sparks.seed=83721;
    sparks.lifetime_ticks=95;sparks.radius=0.025;sparks.color={1,0.45f,0.07f,1};
    auto burst=simulation->create_emitter(sparks);if(!burst)return 4;
    std::uint64_t serial=0;
    for(std::uint64_t tick=1;tick<=180;++tick) {
      if(tick==25||tick==80||tick==135) {
        Event e;e.emitter=burst.value();e.world=config.world;e.source=sparks.source;e.tick=tick;
        e.serial=++serial;e.origin_revision=simulation->origin_revision();e.particles=100;
        e.position={2-simulation->origin().x,1.8-simulation->origin().y,-simulation->origin().z};
        e.payload=Impact{{0,1,0},{71,2}};if(!simulation->submit(e))return 5;
      }
      if(tick==91 && !simulation->rebase({1024,0,0},2))return 6;
      if(!simulation->step(tick))return 7;
      if(tick%15==0) {
        char name[40];std::snprintf(name,sizeof(name),"vfx-%03llu.ppm",static_cast<unsigned long long>(tick));
        if(!frame(output/name,*simulation))return 8;
      }
    }
    const auto stats=simulation->statistics();
    std::ofstream report(output/"summary.txt");
    report<<"RACE bounded CPU cosmetic particle demo\n12 PPM frames, 640x360, 180 fixed ticks at 60 Hz\n"
          <<"offline camera: pixels_per_metre="<<pixels_per_metre<<" ground_y="<<ground_y
          <<"\norigin rebased at tick 91; absolute camera projection preserved\nspawned="<<stats.spawned_particles
          <<"\nexpired="<<stats.expired_particles<<"\ndropped="<<stats.dropped_particles
          <<"\nactive="<<simulation->particles().size()<<"\nrebases="<<stats.rebases<<'\n';
    if(!report)return 8;
    std::cout<<"Wrote 12 CPU PPM frames and summary.txt to "<<output.string()<<'\n';return 0;
  }catch(const std::exception& e){std::cerr<<"VFX demo failed: "<<e.what()<<'\n';return 9;}
}
