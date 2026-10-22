#include "race/vfx/vfx.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace race::vfx {
namespace {
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t random_increment = 0x9e3779b97f4a7c15ULL;
std::atomic<std::uint64_t> next_system{1};
bool finite(double v, double limit = 1.0e12) { return std::isfinite(v) && std::abs(v) <= limit; }
bool finite(Vec3 v, double limit = 1.0e12) { return finite(v.x, limit) && finite(v.y, limit) && finite(v.z, limit); }
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 mul(Vec3 a, double b) { return {a.x*b, a.y*b, a.z*b}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
Vec3 rotate(Orientation q, Vec3 v) {
  const Vec3 axis{q.x,q.y,q.z}; const auto t=mul(cross(axis,v),2);
  return add(v, add(mul(t,q.w),cross(axis,t)));
}
bool valid(Orientation q) {
  if (!finite(q.x,1) || !finite(q.y,1) || !finite(q.z,1) || !finite(q.w,1)) return false;
  return std::abs(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w-1) <= 1.0e-10;
}
std::uint64_t mix(std::uint64_t x) {
  x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;
  x=(x^(x>>27))*0x94d049bb133111ebULL;
  return x^(x>>31);
}
double random_component(std::uint64_t& state) {
  state+=random_increment; // Intentional modulo-2^64 SplitMix stream arithmetic.
  return static_cast<double>(mix(state)>>11)*0x1.0p-52-1;
}
Vec3 payload_velocity(const EventPayload& payload) {
  return std::visit([](const auto& p) -> Vec3 {
    using T=std::decay_t<decltype(p)>;
    if constexpr (std::is_same_v<T,Impact>) return p.impulse_velocity;
    else if constexpr (std::is_same_v<T,Weather>) return p.wind_velocity;
    else return p.velocity;
  },payload);
}
bool same_effect(const Event& a, const Event& b) {
  return a.emitter==b.emitter && a.world==b.world && a.source==b.source &&
    a.tick==b.tick && a.effect_revision==b.effect_revision && a.origin_revision==b.origin_revision &&
    a.position==b.position && a.orientation==b.orientation && a.payload==b.payload;
}
bool valid(const EmitterDesc& d, const Config& c) {
  if (!d.source.valid() || !d.revision || !finite(d.position) || !finite(d.velocity,1e6) ||
      !finite(d.acceleration,1e6) || !finite(d.velocity_spread,1e6) ||
      d.velocity_spread.x<0 || d.velocity_spread.y<0 || d.velocity_spread.z<0 ||
      !d.lifetime_ticks || d.lifetime_ticks>1000000 || d.particles_per_tick>c.max_event_particles ||
      !finite(d.radius,1e6) || d.radius<0) return false;
  for (auto component:d.color) if (!std::isfinite(component) || component<0 || component>1) return false;
  return true;
}
}
struct ParticleSystem::Impl {
  struct Emitter {
    EmitterDesc desc{};
    std::uint64_t generation{1}, random_state{}, last_serial{};
    bool active{}, retired{};
  };
  struct Motion { Vec3 anchor{}, initial_velocity{}, acceleration{}; };
  Config config;
  std::uint64_t identity{}, tick{}, next_particle{1};
  std::vector<Emitter> emitters;
  std::vector<std::size_t> order;
  std::vector<Event> events;
  std::vector<Particle> particles;
  std::vector<Motion> motion;
  Statistics stats;
  explicit Impl(Config c):config(c),tick(c.first_tick),emitters(c.max_emitters) {
    order.reserve(c.max_emitters); events.reserve(c.max_events);
    particles.reserve(c.max_particles); motion.reserve(c.max_particles);
  }
  bool matches(EmitterHandle h) const {
    return h.table==identity && h.slot<emitters.size() && emitters[h.slot].active &&
      emitters[h.slot].generation==h.generation;
  }
  void count(std::uint64_t& counter, std::uint64_t amount) {
    if (maximum-counter<amount) { counter=maximum; stats.counter_saturated=true; }
    else counter+=amount;
  }
  void spawn(Emitter& emitter, Vec3 position, Orientation orientation, Vec3 inherited,
             std::uint32_t requested, std::uint32_t& work, StepReport& report) {
    const auto attempts=std::min(requested,work);
    work-=attempts;
    // Capacity/work drops still advance this emitter's random stream, preserving
    // future source samples without looping over unbudgeted cosmetic work.
    const auto available=config.max_particles-particles.size();
    const auto births=std::min<std::size_t>(attempts,available);
    const auto& d=emitter.desc;
    for (std::size_t n=0;n<births;++n) {
      Vec3 jitter{random_component(emitter.random_state)*d.velocity_spread.x,
                  random_component(emitter.random_state)*d.velocity_spread.y,
                  random_component(emitter.random_state)*d.velocity_spread.z};
      auto velocity=add(rotate(orientation,add(d.velocity,jitter)),inherited);
      particles.push_back({next_particle++,tick,d.source,d.revision,config.origin_revision,
                           position,velocity,0,d.lifetime_ticks,d.radius,d.color});
      motion.push_back({position,velocity,d.acceleration});
    }
    emitter.random_state+=random_increment*3*(requested-static_cast<std::uint32_t>(births));
    report.spawned+=births; report.dropped_particles+=requested-births;
  }
};
ParticleSystem::ParticleSystem(std::unique_ptr<Impl> p):impl_(std::move(p)) {}
ParticleSystem::~ParticleSystem()=default;
Result<std::unique_ptr<ParticleSystem>> ParticleSystem::create(Config c) {
  if (!c.world.valid() || !c.origin_revision || !c.first_tick || !finite(c.origin) ||
      !std::isfinite(c.fixed_step_seconds) || c.fixed_step_seconds<=0 || c.fixed_step_seconds>1 ||
      !c.max_emitters || c.max_emitters>4096 || !c.max_particles || c.max_particles>1048576 ||
      !c.max_events || c.max_events>65536 || !c.max_event_particles || c.max_event_particles>1048576 ||
      !c.max_spawn_attempts_per_tick || c.max_spawn_attempts_per_tick>1048576 ||
      (c.overload!=OverloadPolicy::DropNewest && c.overload!=OverloadPolicy::CoalesceMatching))
    return fail(ErrorCode::InvalidInput,"invalid cosmetic particle limits");
  try {
    auto p=std::make_unique<Impl>(c);
    auto id=next_system.load(std::memory_order_relaxed);
    do {
      if (id==maximum) return fail(ErrorCode::Exhausted,"particle system identities exhausted");
    } while (!next_system.compare_exchange_weak(id,id+1,std::memory_order_relaxed));
    p->identity=id;
    return std::unique_ptr<ParticleSystem>(new ParticleSystem(std::move(p)));
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"particle storage allocation failed"); }
}
Result<EmitterHandle> ParticleSystem::create_emitter(EmitterDesc d) {
  auto& p=*impl_;
  if (!valid(d,p.config)) return fail(ErrorCode::InvalidInput,"invalid cosmetic emitter");
  for (const auto& e:p.emitters) if (e.active && e.desc.source==d.source)
    return fail(ErrorCode::Duplicate,"duplicate cosmetic source identity");
  for (std::size_t i=0;i<p.emitters.size();++i) {
    auto& e=p.emitters[i];
    if (!e.active && !e.retired) {
      e.desc=d; e.last_serial=0;
      e.random_state=mix(d.seed)^mix(d.source.high)^mix(d.source.low+random_increment)^
                     mix(p.config.world.high)^mix(p.config.world.low+2*random_increment);
      e.active=true;
      return EmitterHandle{static_cast<std::uint32_t>(i),e.generation,p.identity};
    }
  }
  return fail(ErrorCode::Exhausted,"cosmetic emitter capacity exhausted");
}
Result<void> ParticleSystem::remove_emitter(EmitterHandle h) {
  auto& p=*impl_;
  if (!p.matches(h)) return fail(ErrorCode::StaleHandle,"stale cosmetic emitter");
  auto& e=p.emitters[h.slot]; e.active=false;
  if (e.generation==maximum) e.retired=true; else ++e.generation;
  for (const auto& event:p.events) if (event.emitter==h) {
    p.count(p.stats.cancelled_events,1); p.count(p.stats.dropped_particles,event.particles);
  }
  std::erase_if(p.events,[&](const Event& event){return event.emitter==h;});
  return {};
}
Result<SubmitResult> ParticleSystem::submit(Event event) {
  auto& p=*impl_;
  if (!p.matches(event.emitter)) return fail(ErrorCode::StaleHandle,"stale cosmetic event emitter");
  auto& e=p.emitters[event.emitter.slot];
  if (event.world!=p.config.world || event.source!=e.desc.source || event.effect_revision!=e.desc.revision ||
      event.origin_revision!=p.config.origin_revision)
    return fail(ErrorCode::StaleHandle,"cosmetic event identity or revision mismatch");
  if (event.tick!=p.tick || !event.serial || event.serial<=e.last_serial)
    return fail(ErrorCode::InvalidInput,"cosmetic event tick or serial is not current");
  if (!event.particles || event.particles>p.config.max_event_particles || !finite(event.position) ||
      !valid(event.orientation) || !finite(payload_velocity(event.payload),1e6))
    return fail(ErrorCode::InvalidInput,"invalid cosmetic event payload");
  e.last_serial=event.serial;
  if (p.config.overload==OverloadPolicy::CoalesceMatching) {
    for (auto& queued:p.events) if (same_effect(queued,event)) {
      const auto accepted=std::min(event.particles,p.config.max_event_particles-queued.particles);
      queued.particles+=accepted;
      p.count(p.stats.coalesced_events,1);
      p.count(p.stats.dropped_particles,event.particles-accepted);
      if (!accepted) p.count(p.stats.dropped_events,1);
      return SubmitResult{Admission::Coalesced,accepted,event.particles-accepted};
    }
  }
  if (p.events.size()==p.config.max_events) {
    p.count(p.stats.dropped_events,1); p.count(p.stats.dropped_particles,event.particles);
    return SubmitResult{Admission::Dropped,0,event.particles};
  }
  p.events.push_back(event); p.count(p.stats.queued_events,1);
  return SubmitResult{Admission::Queued,event.particles,0};
}
Result<StepReport> ParticleSystem::step(std::uint64_t expected_tick) {
  auto& p=*impl_;
  if (expected_tick!=p.tick) return fail(ErrorCode::InvalidInput,"cosmetic fixed tick mismatch");
  if (p.tick==maximum || maximum-p.next_particle<p.config.max_spawn_attempts_per_tick)
    return fail(ErrorCode::Overflow,"cosmetic tick or particle identity exhausted");
  StepReport report; report.tick=p.tick; report.processed_events=p.events.size();
  std::size_t alive=0;
  for (std::size_t i=0;i<p.particles.size();++i) {
    auto particle=p.particles[i]; ++particle.age_ticks;
    if (particle.age_ticks>=particle.lifetime_ticks) { ++report.expired; continue; }
    const auto& motion=p.motion[i];
    const double t=static_cast<double>(particle.age_ticks)*p.config.fixed_step_seconds;
    particle.position=add(motion.anchor,add(mul(motion.initial_velocity,t),mul(motion.acceleration,0.5*t*t)));
    particle.velocity=add(motion.initial_velocity,mul(motion.acceleration,t));
    p.particles[alive]=particle; p.motion[alive]=motion; ++alive;
  }
  p.particles.resize(alive); p.motion.resize(alive);
  p.order.clear();
  for (std::size_t i=0;i<p.emitters.size();++i) if (p.emitters[i].active) p.order.push_back(i);
  std::sort(p.order.begin(),p.order.end(),[&](auto a,auto b){
    return std::tie(p.emitters[a].desc.source.high,p.emitters[a].desc.source.low)<
           std::tie(p.emitters[b].desc.source.high,p.emitters[b].desc.source.low);
  });
  std::uint32_t work=p.config.max_spawn_attempts_per_tick;
  for (auto index:p.order) {
    auto& emitter=p.emitters[index];
    p.spawn(emitter,emitter.desc.position,{}, {},emitter.desc.particles_per_tick,work,report);
  }
  std::sort(p.events.begin(),p.events.end(),[](const Event& a,const Event& b){
    return std::tie(a.source.high,a.source.low,a.serial)<std::tie(b.source.high,b.source.low,b.serial);
  });
  for (const auto& event:p.events) p.spawn(p.emitters[event.emitter.slot],event.position,event.orientation,
                                        payload_velocity(event.payload),event.particles,work,report);
  p.events.clear(); ++p.tick; report.active_particles=p.particles.size();
  p.count(p.stats.spawned_particles,report.spawned); p.count(p.stats.expired_particles,report.expired);
  p.count(p.stats.dropped_particles,report.dropped_particles);
  return report;
}
Result<void> ParticleSystem::rebase(Vec3 origin, std::uint64_t revision) {
  auto& p=*impl_;
  if (!finite(origin) || revision<=p.config.origin_revision)
    return fail(ErrorCode::InvalidInput,"invalid cosmetic origin transaction");
  const auto delta=sub(origin,p.config.origin);
  // Inputs and lifetimes have explicit finite ranges; translating all owned
  // coordinates within these ranges cannot overflow a double. No allocation.
  for (auto& e:p.emitters) if(e.active) e.desc.position=sub(e.desc.position,delta);
  for (auto& event:p.events) { event.position=sub(event.position,delta); event.origin_revision=revision; }
  for (std::size_t i=0;i<p.particles.size();++i) {
    p.particles[i].position=sub(p.particles[i].position,delta);
    p.particles[i].origin_revision=revision; p.motion[i].anchor=sub(p.motion[i].anchor,delta);
  }
  p.config.origin=origin; p.config.origin_revision=revision; p.count(p.stats.rebases,1);
  return {};
}
std::span<const Particle> ParticleSystem::particles() const noexcept { return impl_->particles; }
Bounds ParticleSystem::bounds() const noexcept {
  Bounds result;
  const auto infinity=std::numeric_limits<double>::infinity();
  for (const auto& particle:impl_->particles) {
    const auto p=particle.position; const auto r=particle.radius;
    const Vec3 lo{std::nextafter(p.x-r,-infinity),std::nextafter(p.y-r,-infinity),std::nextafter(p.z-r,-infinity)};
    const Vec3 hi{std::nextafter(p.x+r,infinity),std::nextafter(p.y+r,infinity),std::nextafter(p.z+r,infinity)};
    if (result.empty) { result={false,lo,hi}; continue; }
    result.minimum={std::min(result.minimum.x,lo.x),std::min(result.minimum.y,lo.y),std::min(result.minimum.z,lo.z)};
    result.maximum={std::max(result.maximum.x,hi.x),std::max(result.maximum.y,hi.y),std::max(result.maximum.z,hi.z)};
  }
  return result;
}
Statistics ParticleSystem::statistics() const noexcept { return impl_->stats; }
std::uint64_t ParticleSystem::next_tick() const noexcept { return impl_->tick; }
std::uint64_t ParticleSystem::origin_revision() const noexcept { return impl_->config.origin_revision; }
Vec3 ParticleSystem::origin() const noexcept { return impl_->config.origin; }
} // namespace race::vfx
