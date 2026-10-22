#include "race/audio/audio.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>
using namespace race;
using namespace race::audio;
namespace {
void little(std::ostream& stream,std::uint32_t value,unsigned count) {
  for(unsigned i=0;i<count;++i)stream.put(static_cast<char>((value>>(i*8))&255));
}
ClipLease tone(double frequency,std::uint32_t rate) {
  std::vector<std::int16_t> pcm(rate);
  for(std::size_t i=0;i<pcm.size();++i) {
    const double t=static_cast<double>(i)/rate;
    const double envelope=std::min(1.0,t*100)*std::min(1.0,(1-t)*100);
    pcm[i]=static_cast<std::int16_t>(std::round(7000*envelope*std::sin(6.283185307179586*frequency*t)));
  }
  auto result=make_pcm16_clip(rate,1,pcm);return result?result.value():ClipLease{};
}
}
int main(int argc,char** argv) {
  if(argc!=2){std::fprintf(stderr,"usage: race_audio_demo OUTPUT.wav\n");return 2;}
  auto low=tone(220,24000),high=tone(440,44100);if(!low||!high)return 3;
  Mixer mixer;Play left;left.spatial=true;left.position={-2,0,0};left.minimum_distance=1;left.maximum_distance=8;
  Play right=left;right.position={2,0,0};right.start_frame=24000;right.bus=1;
  if(!mixer.set_bus_gain(1,.75)||!mixer.play(low,left)||!mixer.play(high,right))return 4;
  constexpr std::uint32_t frames=96000,bytes=frames*4;
  std::ofstream file(argv[1],std::ios::binary|std::ios::trunc);if(!file)return 5;
  file.write("RIFF",4);little(file,36+bytes,4);file.write("WAVEfmt ",8);little(file,16,4);
  little(file,1,2);little(file,2,2);little(file,output_rate,4);little(file,output_rate*4,4);
  little(file,4,2);little(file,16,2);file.write("data",4);little(file,bytes,4);
  std::array<std::int16_t,512*2> block{};
  for(std::uint32_t at=0;at<frames;) {
    const auto count=std::min<std::uint32_t>(512,frames-at);
    if(!mixer.mix(std::span(block).first(count*2)))return 6;
    for(std::size_t i=0;i<count*2;++i)little(file,static_cast<std::uint16_t>(block[i]),2);
    at+=count;
  }
  mixer.collect_finished();file.close();if(!file)return 7;
  std::printf("offline stereo PCM16: %u frames at %u Hz, %u bytes; no device playback claim\n",frames,output_rate,44+bytes);
  return 0;
}
