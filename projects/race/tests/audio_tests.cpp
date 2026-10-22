#include "race/audio/audio.hpp"
#include "test.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace { std::atomic<std::size_t> allocations{0}; }
void* operator new(std::size_t n) {
  allocations.fetch_add(1,std::memory_order_relaxed);
  if(void* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
using namespace race;
using namespace race::audio;
namespace {
void put(std::vector<std::byte>& b,std::size_t at,std::uint32_t value,std::size_t count) {
  for(std::size_t i=0;i<count;++i)b[at+i]=std::byte((value>>(i*8))&255);
}
void tag(std::vector<std::byte>& b,std::size_t at,const char* s) {
  for(std::size_t i=0;i<4;++i)b[at+i]=std::byte(s[i]);
}
std::vector<std::byte> wave(std::span<const std::int16_t> s,std::uint32_t rate=48000,std::uint16_t channels=1) {
  std::vector<std::byte>b(44+s.size()*2);tag(b,0,"RIFF");put(b,4,static_cast<std::uint32_t>(b.size()-8),4);tag(b,8,"WAVE");
  tag(b,12,"fmt ");put(b,16,16,4);put(b,20,1,2);put(b,22,channels,2);put(b,24,rate,4);
  put(b,28,rate*channels*2u,4);put(b,32,channels*2u,2);put(b,34,16,2);tag(b,36,"data");
  put(b,40,static_cast<std::uint32_t>(s.size()*2),4);
  for(std::size_t i=0;i<s.size();++i)put(b,44+i*2,static_cast<std::uint16_t>(s[i]),2);
  return b;
}
ClipLease clip(std::initializer_list<std::int16_t> samples,std::uint32_t rate=48000,std::uint16_t channels=1) {
  auto result=make_pcm16_clip(rate,channels,std::span(samples.begin(),samples.size()));CHECK(result);return result.value();
}
void decode() {
  const std::array<std::int16_t,4> source{-32768,-1,0,32767};auto bytes=wave(source,44100,2);
  auto c=decode_pcm16_wave(bytes);CHECK(c);CHECK(c.value()->channels()==2);CHECK(c.value()->sample_rate()==44100);
  CHECK(c.value()->frames()==2);CHECK(std::equal(source.begin(),source.end(),c.value()->samples().begin()));
  for(std::size_t n=0;n<bytes.size();++n)CHECK(!decode_pcm16_wave(std::span(bytes).first(n)));
  auto bad=bytes;bad.push_back(std::byte{});CHECK(!decode_pcm16_wave(bad));
  for(auto at:{0u,8u,20u,22u,28u,32u,34u}) {bad=bytes;bad[at]=std::byte{255};CHECK(!decode_pcm16_wave(bad));}
  bad=bytes;put(bad,40,UINT32_MAX,4);CHECK(!decode_pcm16_wave(bad));
  bad=bytes;put(bad,24,0,4);CHECK(!decode_pcm16_wave(bad));
  CHECK_ERROR(decode_pcm16_wave(bytes,{44,1920000}),ErrorCode::Exhausted);
  CHECK_ERROR(decode_pcm16_wave(bytes,{1024,1}),ErrorCode::Exhausted);
  CHECK(!make_pcm16_clip(0,1,source));CHECK(!make_pcm16_clip(48000,3,source));
  CHECK(!make_pcm16_clip(48000,2,std::span(source).first(3)));
  // An odd optional chunk requires padding and does not change samples.
  auto with_junk=bytes;with_junk.insert(with_junk.begin()+12,10,std::byte{});
  tag(with_junk,12,"JUNK");put(with_junk,16,1,4);with_junk[20]=std::byte{7};
  put(with_junk,4,static_cast<std::uint32_t>(with_junk.size()-8),4);
  CHECK(decode_pcm16_wave(with_junk));
  auto extended=bytes;extended.insert(extended.begin()+36,2,std::byte{});put(extended,16,18,4);
  put(extended,4,static_cast<std::uint32_t>(extended.size()-8),4);CHECK(decode_pcm16_wave(extended));
  extended[36]=std::byte{2};CHECK_ERROR(decode_pcm16_wave(extended),ErrorCode::Unsupported);
  auto many=bytes;
  for(unsigned i=0;i<63;++i){const auto at=many.size();many.resize(at+8);tag(many,at,"JUNK");}
  put(many,4,static_cast<std::uint32_t>(many.size()-8),4);CHECK_ERROR(decode_pcm16_wave(many),ErrorCode::Exhausted);
  // Duplicate chunks and format extension claims cannot redirect decoded data.
  bad=bytes;bad.insert(bad.end(),bytes.begin()+12,bytes.begin()+36);
  put(bad,4,static_cast<std::uint32_t>(bad.size()-8),4);
  CHECK_ERROR(decode_pcm16_wave(bad),ErrorCode::Duplicate);
  bad=bytes;bad.insert(bad.end(),bytes.begin()+36,bytes.end());put(bad,4,static_cast<std::uint32_t>(bad.size()-8),4);
  CHECK_ERROR(decode_pcm16_wave(bad),ErrorCode::Duplicate);
  bad=bytes;bad.resize(bad.size()+9);tag(bad,bytes.size(),"JUNK");put(bad,bytes.size()+4,1,4);
  put(bad,4,static_cast<std::uint32_t>(bad.size()-8),4);CHECK(!decode_pcm16_wave(bad));
}
void scheduled_and_leases() {
  Mixer m;auto c=clip({1000,2000,3000});std::weak_ptr<const Clip> weak=c;
  Play p;p.start_frame=2;auto h=m.play(c,p);CHECK(h);c.reset();CHECK(!weak.expired());
  std::array<std::int16_t,12> output{};const auto before=allocations.load();auto r=m.mix(output);
  CHECK(allocations.load()==before);CHECK(r);CHECK(r.value().started==1);CHECK(r.value().finished==1);
  const std::array<std::int16_t,12> expected{0,0,0,0,1000,1000,2000,2000,3000,3000,0,0};CHECK(output==expected);
  CHECK(m.frame()==6);CHECK(!weak.expired());CHECK_ERROR(m.stop(h.value(),6),ErrorCode::StaleHandle);
  CHECK(m.collect_finished()==1);CHECK(weak.expired());CHECK(m.statistics().retained_bytes==0);
  CHECK(m.statistics().resident_voices==0);CHECK(m.collect_finished()==0);
  p.start_frame=6;auto next=m.play(clip({12}),p);CHECK(next);CHECK(next.value()!=h.value());
  CHECK_ERROR(m.stop(h.value(),6),ErrorCode::StaleHandle);
  Mixer foreign;CHECK_ERROR(foreign.stop(next.value(),0),ErrorCode::StaleHandle);
}
void resampling_and_loop() {
  Mixer half;CHECK(half.play(clip({0,1000,2000},24000)));
  std::array<std::int16_t,12> out{};CHECK(half.mix(out));
  const std::array<std::int16_t,12> expected{0,0,500,500,1000,1000,1500,1500,2000,2000,2000,2000};CHECK(out==expected);
  Mixer fast;CHECK(fast.play(clip({10,20,30,40},96000)));std::array<std::int16_t,6> skipped{};CHECK(fast.mix(skipped));
  CHECK((skipped==std::array<std::int16_t,6>{10,10,30,30,0,0}));
  Mixer fractional;CHECK(fractional.play(clip({0,1000},44100)));CHECK(fractional.mix(skipped));
  CHECK((skipped==std::array<std::int16_t,6>{0,0,919,919,1000,1000}));
  Mixer loop;Play p;p.loop=true;auto h=loop.play(clip({0,1000},24000),p);CHECK(h);CHECK(loop.stop(h.value(),5));
  CHECK(loop.mix(out));CHECK((out==std::array<std::int16_t,12>{0,0,500,500,1000,1000,500,500,0,0,0,0}));
  CHECK(loop.collect_finished()==1);
  // Rational phase and absolute sample scheduling must not depend on block splits.
  Mixer whole,parts;p.start_frame=3;p.loop=true;auto source=clip({-3000,0,2000,9000,-7000},44100);
  CHECK(whole.play(source,p));CHECK(parts.play(source,p));
  std::array<std::int16_t,200> a{},b{};CHECK(whole.mix(a));
  CHECK(parts.mix(std::span(b).first(14)));CHECK(parts.mix(std::span(b).subspan(14,52)));CHECK(parts.mix(std::span(b).subspan(66)));
  CHECK(a==b);CHECK(whole.frame()==parts.frame());
}
void buses_and_clipping() {
  Mixer m;CHECK(m.set_bus_gain(1,.5));Play p;p.bus=1;p.gain=.5;
  CHECK(m.play(clip({1000,-1000}),p));CHECK(m.play(clip({2000,-2000})));
  std::array<std::int16_t,4> out{};CHECK(m.mix(out));CHECK((out==std::array<std::int16_t,4>{2250,2250,-2250,-2250}));
  Mixer saturation;CHECK(saturation.play(clip({30000,-30000})));CHECK(saturation.play(clip({30000,-30000})));
  auto r=saturation.mix(out);CHECK(r);CHECK((out==std::array<std::int16_t,4>{32767,32767,-32768,-32768}));
  CHECK(r.value().clipped_samples==4);CHECK(saturation.statistics().clipped_samples==4);
  Mixer stereo;CHECK(stereo.play(clip({10,-20,30,-40},48000,2)));CHECK(stereo.mix(out));
  CHECK((out==std::array<std::int16_t,4>{10,-20,30,-40}));
  CHECK_ERROR(m.set_bus_gain(4,1),ErrorCode::InvalidInput);CHECK_ERROR(m.set_bus_gain(0,-1),ErrorCode::InvalidInput);
  CHECK_ERROR(m.set_bus_gain(0,std::numeric_limits<double>::quiet_NaN()),ErrorCode::InvalidInput);
}
void admission_and_atomic_rejects() {
  Mixer m;CHECK(m.set_limits({1,64}));Play p;p.loop=true;p.priority=5;auto first=m.play(clip({100}),p);CHECK(first);
  CHECK_ERROR(m.play(clip({200}),p),ErrorCode::QueueFull);
  p.admission=Admission::StealLowerPriority;CHECK_ERROR(m.play(clip({200}),p),ErrorCode::QueueFull);
  p.priority=6;auto second=m.play(clip({200}),p);CHECK(second);CHECK_ERROR(m.stop(first.value(),0),ErrorCode::StaleHandle);
  CHECK(m.statistics().stolen==1);CHECK(m.statistics().resident_voices==1);CHECK(m.statistics().rejected==2);
  CHECK_ERROR(m.set_limits({2,64}),ErrorCode::Busy);
  Mixer ties;CHECK(ties.set_limits({2,64}));p.priority=1;
  auto older=ties.play(clip({1}),p),newer=ties.play(clip({2}),p);CHECK(older);CHECK(newer);
  p.priority=2;CHECK(ties.play(clip({3}),p));CHECK_ERROR(ties.stop(older.value(),0),ErrorCode::StaleHandle);
  CHECK(ties.stop(newer.value(),0));
  Mixer bounded;CHECK(bounded.set_limits({1,2}));auto c=clip({100});auto h=bounded.play(c);CHECK(h);
  p.priority=10;CHECK_ERROR(bounded.play(clip({200,300}),p),ErrorCode::Exhausted);
  std::array<std::int16_t,2> output{};CHECK(bounded.mix(output));CHECK(output[0]==100);
  CHECK(bounded.statistics().stolen==0);CHECK(bounded.statistics().admitted==1);
  Mixer invalid;auto valid=clip({50});p=Play{};p.gain=std::numeric_limits<double>::infinity();
  CHECK_ERROR(invalid.play(valid,p),ErrorCode::InvalidInput);CHECK(invalid.frame()==0);
  p=Play{};p.position.x=std::numeric_limits<double>::quiet_NaN();CHECK(!invalid.play(valid,p));
  std::array<std::int16_t,3> bad{11,22,33};CHECK(!invalid.mix(bad));CHECK((bad==std::array<std::int16_t,3>{11,22,33}));
  CHECK(invalid.frame()==0);CHECK(invalid.mix({}));CHECK(invalid.frame()==0);
  std::vector<std::int16_t> oversized((max_block_frames+1)*2,17);CHECK(!invalid.mix(oversized));CHECK(oversized.front()==17);
  CHECK(!invalid.set_limits({0,1}));CHECK(!invalid.set_limits({17,1}));CHECK(!invalid.set_limits({1,0}));
}
void spatial_and_origin() {
  Mixer a,b;Play p;p.spatial=true;p.position={5,0,0};p.minimum_distance=0;p.maximum_distance=10;p.loop=true;
  auto c=clip({1000});auto h=a.play(c,p);CHECK(h);CHECK(b.play(c,p));
  std::array<std::int16_t,2> original{},rebased{};CHECK(a.mix(original));CHECK(original[0]==0);CHECK(original[1]==500);
  CHECK(b.rebase({1000000,2000000,-3000000},2));CHECK(b.mix(rebased));CHECK(original==rebased);
  CHECK(b.listener().origin_revision==2);CHECK(!b.set_listener(Listener{}));
  CHECK_ERROR(b.rebase({0,0,0},2),ErrorCode::InvalidInput);
  const auto old=b.listener();CHECK(!b.rebase({1e12,0,0},3));CHECK(b.listener().origin_revision==old.origin_revision);
  CHECK(!a.set_position(h.value(),{-5,0,0},2));CHECK(a.set_position(h.value(),{-5,0,0},1));
  CHECK(a.mix(original));CHECK(original[0]==500);CHECK(original[1]==0);
  CHECK(a.set_position(h.value(),{0,0,0},1));CHECK(a.mix(original));CHECK(original[0]==1000);CHECK(original[1]==1000);
  CHECK(a.set_position(h.value(),{0,0,10},1));CHECK(a.mix(original));CHECK(original[0]==0);CHECK(original[1]==0);
  CHECK(!a.set_listener({{},{0,0,0},1}));p.origin_revision=9;CHECK(!a.play(c,p));
  p.origin_revision=1;CHECK(!a.play(clip({1,2},48000,2),p));
}
}
int main() {
  decode();scheduled_and_leases();resampling_and_loop();buses_and_clipping();admission_and_atomic_rejects();spatial_and_origin();
  std::puts("audio: bounded WAV, exact sample scheduling/resampling, mixer ownership, spatial origin and admission checks passed");
}
