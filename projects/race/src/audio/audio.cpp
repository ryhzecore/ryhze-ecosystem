#include "race/audio/audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace race::audio {
namespace {
std::atomic<std::uint64_t> next_owner{1};
void add(std::uint64_t& counter, std::uint64_t amount = 1) noexcept {
  counter += std::min(amount, UINT64_MAX - counter);
}
bool valid_limits(DecodeLimits l) {
  return l.input_bytes >= 44 && l.input_bytes <= 8 * 1024 * 1024 &&
         l.frames > 0 && l.frames <= 1920000;
}
bool valid_rate(std::uint32_t rate) { return rate >= 8000 && rate <= 192000; }
bool finite(Vec3 v, double bound = 1e9) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::abs(v.x) <= bound && std::abs(v.y) <= bound && std::abs(v.z) <= bound;
}
Vec3 plus(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 minus(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
bool gain_valid(double g) { return std::isfinite(g) && g >= 0 && g <= 4; }
bool listener_valid(Listener l) {
  const auto r=l.right;
  return l.origin_revision && finite(l.position) && finite(r,1) &&
         std::abs(r.x*r.x+r.y*r.y+r.z*r.z-1) <= 1e-12;
}
std::array<double,2> spatial_gains(const Play& p, const Listener& listener) {
  if (!p.spatial) return {1,1};
  const auto d=minus(p.position,listener.position);
  const double distance=std::hypot(d.x,d.y,d.z);
  const double attenuation=distance<=p.minimum_distance?1:
      std::clamp((p.maximum_distance-distance)/(p.maximum_distance-p.minimum_distance),0.0,1.0);
  const double pan=distance==0?0:std::clamp(
      (d.x*listener.right.x+d.y*listener.right.y+d.z*listener.right.z)/distance,-1.0,1.0);
  return {attenuation*std::min(1.0,1-pan),attenuation*std::min(1.0,1+pan)};
}
std::uint32_t read(std::span<const std::byte> bytes,std::size_t at,std::size_t n) {
  std::uint32_t value=0;
  for(std::size_t i=0;i<n;++i) value|=std::uint32_t(std::to_integer<unsigned>(bytes[at+i]))<<(8*i);
  return value;
}
bool tag(std::span<const std::byte> bytes,std::size_t at,const char* text) {
  return std::memcmp(bytes.data()+at,text,4)==0;
}
}
Clip::Clip(std::uint32_t rate,std::uint16_t channels,std::span<const std::int16_t> samples)
    :rate_(rate),channels_(channels),samples_(samples.begin(),samples.end()) {}
Result<ClipLease> make_pcm16_clip(std::uint32_t rate,std::uint16_t channels,
    std::span<const std::int16_t> samples,DecodeLimits limits) {
  if(!valid_limits(limits)||!valid_rate(rate)||(channels!=1&&channels!=2)||
      samples.empty()||samples.size()%channels)
    return fail(ErrorCode::InvalidInput,"invalid bounded PCM clip");
  if(samples.size()/channels>limits.frames||samples.size()>limits.input_bytes/2)
    return fail(ErrorCode::Exhausted,"PCM clip exceeds decode budget");
  try { return ClipLease(new Clip(rate,channels,samples)); }
  catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"PCM clip allocation failed"); }
}
Result<ClipLease> decode_pcm16_wave(std::span<const std::byte> bytes,DecodeLimits limits) {
  if(!valid_limits(limits)) return fail(ErrorCode::InvalidInput,"invalid decoder limits");
  if(bytes.size()>limits.input_bytes) return fail(ErrorCode::Exhausted,"WAVE input exceeds budget");
  if(bytes.size()<12||!tag(bytes,0,"RIFF")||!tag(bytes,8,"WAVE")||
      std::uint64_t(read(bytes,4,4))+8!=bytes.size())
    return fail(ErrorCode::InvalidInput,"invalid exact RIFF WAVE extent");
  std::size_t cursor=12,data_at=0,data_bytes=0,chunks=0;
  std::uint32_t rate=0;std::uint16_t channels=0;bool format=false,data=false;
  while(cursor<bytes.size()) {
    if(++chunks>64) return fail(ErrorCode::Exhausted,"WAVE chunk count exceeds budget");
    if(bytes.size()-cursor<8) return fail(ErrorCode::InvalidInput,"truncated WAVE chunk header");
    const std::size_t count=read(bytes,cursor+4,4),body=cursor+8;
    if(count>bytes.size()-body) return fail(ErrorCode::InvalidInput,"truncated WAVE chunk");
    if(tag(bytes,cursor,"fmt ")) {
      if(format) return fail(ErrorCode::Duplicate,"duplicate WAVE format");
      if((count!=16&&count!=18)||read(bytes,body,2)!=1||
          (count==18&&read(bytes,body+16,2)!=0))
        return fail(ErrorCode::Unsupported,"only canonical PCM16 WAVE supported");
      channels=static_cast<std::uint16_t>(read(bytes,body+2,2));rate=read(bytes,body+4,4);
      if((channels!=1&&channels!=2)||!valid_rate(rate)||read(bytes,body+14,2)!=16)
        return fail(ErrorCode::Unsupported,"unsupported WAVE PCM configuration");
      if(read(bytes,body+12,2)!=channels*2u||read(bytes,body+8,4)!=rate*channels*2u)
        return fail(ErrorCode::InvalidInput,"inconsistent WAVE block or byte rate");
      format=true;
    } else if(tag(bytes,cursor,"data")) {
      if(data) return fail(ErrorCode::Duplicate,"duplicate WAVE data");
      data=true;data_at=body;data_bytes=count;
    }
    cursor=body+count;
    if(count%2) {
      if(cursor==bytes.size()) return fail(ErrorCode::InvalidInput,"missing RIFF chunk padding");
      ++cursor;
    }
  }
  if(!format||!data||!data_bytes||data_bytes%(channels*2u))
    return fail(ErrorCode::InvalidInput,"missing or partial PCM frames");
  if(data_bytes/(channels*2u)>limits.frames)
    return fail(ErrorCode::Exhausted,"PCM frame count exceeds budget");
  try {
    std::vector<std::int16_t> samples(data_bytes/2);
    for(std::size_t i=0;i<samples.size();++i) {
      const auto raw=read(bytes,data_at+i*2,2);
      samples[i]=static_cast<std::int16_t>(raw>=32768?static_cast<int>(raw)-65536:static_cast<int>(raw));
    }
    return make_pcm16_clip(rate,channels,samples,limits);
  } catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"WAVE decode allocation failed"); }
}
Mixer::Mixer() noexcept {
  auto value=next_owner.load();
  while(value!=UINT64_MAX) if(next_owner.compare_exchange_weak(value,value+1)) {owner_=value;break;}
}
Result<void> Mixer::set_limits(Limits l) noexcept {
  if(!l.voices||l.voices>max_voices||!l.retained_bytes||l.retained_bytes>64*1024*1024)
    return fail(ErrorCode::InvalidInput,"invalid mixer limits");
  if(stats_.resident_voices) return fail(ErrorCode::Busy,"voices still retain clips");
  limits_=l;return {};
}
Result<std::size_t> Mixer::resolve(VoiceHandle h) const noexcept {
  if(h.owner!=owner_||!owner_||h.slot>=voices_.size()||!h.generation||
      voices_[h.slot].generation!=h.generation||!voices_[h.slot].clip||voices_[h.slot].finished)
    return fail(ErrorCode::StaleHandle,"stale or foreign audio voice");
  return std::size_t(h.slot);
}
Result<VoiceHandle> Mixer::play(ClipLease clip,Play p) noexcept {
  const auto reject=[this](ErrorCode code,const char* message)->Result<VoiceHandle>{add(stats_.rejected);return fail(code,message);};
  if(!clip||!p.source.valid()||p.start_frame<frame_||!gain_valid(p.gain)||p.bus>=max_buses||
      !finite(p.position)||!p.origin_revision||
      !std::isfinite(p.minimum_distance)||!std::isfinite(p.maximum_distance)||
      p.minimum_distance<0||p.maximum_distance<=p.minimum_distance||p.maximum_distance>1e9||
      (p.admission!=Admission::Reject&&p.admission!=Admission::StealLowerPriority))
    return reject(ErrorCode::InvalidInput,"invalid audio play request");
  if(p.spatial&&(p.origin_revision!=listener_.origin_revision||clip->channels()!=1))
    return reject(ErrorCode::Unsupported,"spatial voice requires current origin and mono PCM");
  if(!owner_||serial_==UINT64_MAX) return reject(ErrorCode::Exhausted,"audio identity space exhausted");
  std::size_t slot=max_voices;
  for(std::size_t i=0;i<limits_.voices;++i)
    if(!voices_[i].clip&&voices_[i].generation!=UINT64_MAX){slot=i;break;}
  bool stealing=false;
  if(slot==max_voices&&p.admission==Admission::StealLowerPriority) {
    for(std::size_t i=0;i<limits_.voices;++i) {
      const auto& v=voices_[i];
      if(v.finished||v.generation==UINT64_MAX||v.play.priority>=p.priority)continue;
      if(slot==max_voices||v.play.priority<voices_[slot].play.priority||
          (v.play.priority==voices_[slot].play.priority&&v.serial<voices_[slot].serial))slot=i;
    }
    stealing=slot!=max_voices;
  }
  if(slot==max_voices) return reject(ErrorCode::QueueFull,"audio voice capacity exhausted");
  const auto released=voices_[slot].clip?voices_[slot].clip->bytes():0;
  if(clip->bytes()>limits_.retained_bytes||stats_.retained_bytes-released>limits_.retained_bytes-clip->bytes())
    return reject(ErrorCode::Exhausted,"audio retained clip budget exhausted");
  const auto gains=spatial_gains(p,listener_);auto& v=voices_[slot];
  const auto generation=v.generation+1;
  v=Voice{};v.clip=std::move(clip);v.play=p;v.generation=generation;v.serial=++serial_;
  v.left=gains[0];v.right=gains[1];
  stats_.retained_bytes=stats_.retained_bytes-released+v.clip->bytes();
  if(stealing)add(stats_.stolen);else ++stats_.resident_voices;
  add(stats_.admitted);return VoiceHandle{owner_,generation,static_cast<std::uint32_t>(slot)};
}
Result<void> Mixer::stop(VoiceHandle h,std::uint64_t exclusive_frame) noexcept {
  auto slot=resolve(h);if(!slot)return slot.error();
  if(exclusive_frame<frame_)return fail(ErrorCode::InvalidInput,"audio stop is in the past");
  voices_[slot.value()].stop_frame=exclusive_frame;return {};
}
Result<void> Mixer::set_bus_gain(std::uint8_t bus,double gain) noexcept {
  if(bus>=max_buses||!gain_valid(gain))return fail(ErrorCode::InvalidInput,"invalid audio bus gain");
  buses_[bus]=gain;return {};
}
Result<void> Mixer::set_listener(Listener next) noexcept {
  if(!listener_valid(next)||next.origin_revision!=listener_.origin_revision)
    return fail(ErrorCode::InvalidInput,"invalid listener or origin revision");
  for(auto& v:voices_) if(v.clip&&!v.finished) {const auto g=spatial_gains(v.play,next);v.left=g[0];v.right=g[1];}
  listener_=next;return {};
}
Result<void> Mixer::set_position(VoiceHandle h,Vec3 position,std::uint64_t revision) noexcept {
  auto slot=resolve(h);if(!slot)return slot.error();
  if(!finite(position)||revision!=listener_.origin_revision)
    return fail(ErrorCode::InvalidInput,"invalid audio source coordinates");
  auto& v=voices_[slot.value()];auto p=v.play;p.position=position;p.origin_revision=revision;
  const auto g=spatial_gains(p,listener_);v.play=p;v.left=g[0];v.right=g[1];return {};
}
Result<void> Mixer::rebase(Vec3 origin,std::uint64_t revision) noexcept {
  if(!finite(origin,1e12)||revision<=listener_.origin_revision)
    return fail(ErrorCode::InvalidInput,"invalid audio origin transaction");
  const auto shift=minus(origin_,origin);const auto listener=plus(listener_.position,shift);
  if(!finite(listener))return fail(ErrorCode::Exhausted,"listener exceeds local coordinate budget");
  std::array<Vec3,max_voices> positions{};
  for(std::size_t i=0;i<voices_.size();++i)if(voices_[i].clip) {
    positions[i]=plus(voices_[i].play.position,shift);
    if(!finite(positions[i]))return fail(ErrorCode::Exhausted,"voice exceeds local coordinate budget");
  }
  for(std::size_t i=0;i<voices_.size();++i)if(voices_[i].clip) {
    voices_[i].play.position=positions[i];voices_[i].play.origin_revision=revision;
  }
  listener_.position=listener;listener_.origin_revision=revision;origin_=origin;
  // Relative geometry is unchanged: retain exact cached gains across rebasing.
  return {};
}
Result<MixReport> Mixer::mix(std::span<std::int16_t> output) noexcept {
  if(output.size()%2||output.size()/2>max_block_frames)
    return fail(ErrorCode::InvalidInput,"invalid bounded stereo output span");
  const auto frames=output.size()/2;
  if(frames>UINT64_MAX-frame_)return fail(ErrorCode::Overflow,"audio clock exhausted");
  MixReport report;report.frames=frames;
  for(std::size_t f=0;f<frames;++f) {
    double left=0,right=0;const auto tick=frame_+f;
    for(auto& v:voices_) {
      if(!v.clip||v.finished)continue;
      if(tick>=v.stop_frame){v.finished=true;++report.finished;add(stats_.completed);continue;}
      if(tick<v.play.start_frame)continue;
      if(!v.started){v.started=true;++report.started;}
      const auto& clip=*v.clip;const auto count=clip.frames();
      const auto index=static_cast<std::size_t>(v.position/output_rate);
      const auto next=index+1<count?index+1:(v.play.loop?0:index);
      const double fraction=double(v.position%output_rate)/output_rate;
      const auto samples=clip.samples();const auto channels=clip.channels();
      const auto sample=[&](std::size_t channel){
        const double a=samples[index*channels+channel],b=samples[next*channels+channel];
        return a+(b-a)*fraction;
      };
      const double gain=v.play.gain*buses_[v.play.bus];
      left+=sample(0)*gain*v.left;right+=sample(channels==1?0:1)*gain*v.right;
      v.position+=clip.sample_rate();
      const std::uint64_t end=count*std::uint64_t(output_rate);
      if(v.position>=end) {
        if(v.play.loop)v.position%=end;
        else {v.finished=true;++report.finished;add(stats_.completed);}
      }
    }
    for(std::size_t channel=0;channel<2;++channel) {
      const double value=std::round(channel?right:left);
      if(value>32767||value< -32768)++report.clipped_samples;
      output[f*2+channel]=static_cast<std::int16_t>(std::clamp(value,-32768.0,32767.0));
    }
  }
  frame_+=frames;add(stats_.clipped_samples,report.clipped_samples);
  for(const auto& v:voices_)if(v.clip&&!v.finished&&v.started)++report.active;
  return report;
}
std::size_t Mixer::collect_finished() noexcept {
  std::size_t count=0;
  for(auto& v:voices_)if(v.clip&&v.finished) {
    stats_.retained_bytes-=v.clip->bytes();--stats_.resident_voices;
    v.clip.reset();++count;
  }
  return count;
}
} // namespace race::audio
