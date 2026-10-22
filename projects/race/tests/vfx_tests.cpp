#include "race/vfx/vfx.hpp"
#include "test.hpp"
#include <cmath>
#include <limits>
#include <vector>
using namespace race;
using namespace race::vfx;
namespace {
std::unique_ptr<ParticleSystem> system(Config c={}) {
  auto result=ParticleSystem::create(c); CHECK(result); return std::move(result).value();
}
EmitterDesc description(std::uint64_t source=1) {
  EmitterDesc d; d.source={10,source}; d.seed=372; d.lifetime_ticks=4; return d;
}
EmitterHandle emitter(ParticleSystem& s,EmitterDesc d=description()) {
  auto result=s.create_emitter(d); CHECK(result); return result.value();
}
Event event(ParticleSystem& s,EmitterHandle h,std::uint64_t serial=1,std::uint32_t count=3,std::uint64_t source=1) {
  Event e; e.emitter=h; e.world={1,1}; e.source={10,source}; e.serial=serial;
  e.tick=s.next_tick(); e.origin_revision=s.origin_revision(); e.particles=count;
  e.payload=Impact{{0,2,0},{4,5}}; return e;
}
bool near(double a,double b,double epsilon=1e-9) {return std::abs(a-b)<epsilon;}
bool near(Vec3 a,Vec3 b) {return near(a.x,b.x)&&near(a.y,b.y)&&near(a.z,b.z);}
void equal(const Particle& a,const Particle& b,bool identities=true) {
  if(identities) CHECK(a.id==b.id);
  CHECK(a.source==b.source); CHECK(a.birth_tick==b.birth_tick);
  CHECK(a.effect_revision==b.effect_revision); CHECK(a.position==b.position);
  CHECK(a.velocity==b.velocity); CHECK(a.age_ticks==b.age_ticks);
  CHECK(a.lifetime_ticks==b.lifetime_ticks); CHECK(a.radius==b.radius); CHECK(a.color==b.color);
}
void deterministic_replay() {
  auto a=system(), b=system(); auto a1=emitter(*a),a2=emitter(*a,description(2));
  // Source and event order across independent emitters cannot change evaluation.
  auto b2=emitter(*b,description(2)),b1=emitter(*b);
  for(std::uint64_t tick=1;tick<=12;++tick) {
    CHECK(a->submit(event(*a,a1,tick))); CHECK(a->submit(event(*a,a2,tick,2,2)));
    CHECK(b->submit(event(*b,b2,tick,2,2))); CHECK(b->submit(event(*b,b1,tick)));
    auto ar=a->step(tick);auto br=b->step(tick);CHECK(ar&&br);
    CHECK(ar.value().spawned==br.value().spawned);CHECK(ar.value().expired==br.value().expired);
    CHECK(a->particles().size()==b->particles().size());
    for(std::size_t i=0;i<a->particles().size();++i)equal(a->particles()[i],b->particles()[i]);
    CHECK_ERROR(a->step(tick),ErrorCode::InvalidInput);
    auto stale=event(*a,a1,tick+100);stale.tick=tick;
    CHECK_ERROR(a->submit(stale),ErrorCode::InvalidInput);
  }
}
void independent_streams() {
  auto a=system(),b=system();auto ah=emitter(*a),bh=emitter(*b);
  auto other=description(2);other.particles_per_tick=2;CHECK(b->create_emitter(other));
  for(std::uint64_t tick=1;tick<=8;++tick) {
    CHECK(a->submit(event(*a,ah,tick)));CHECK(b->submit(event(*b,bh,tick)));
    CHECK(a->step(tick));CHECK(b->step(tick));
    std::vector<Particle> source;
    for(auto p:b->particles())if(p.source==PersistentId{10,1})source.push_back(p);
    CHECK(source.size()==a->particles().size());
    for(std::size_t i=0;i<source.size();++i)equal(a->particles()[i],source[i],false);
  }
  // Dropped births consume their stream positions without unbudgeted loops.
  Config small;small.max_particles=2;small.max_spawn_attempts_per_tick=2;
  auto capped=system(small),full=system();auto d=description();d.lifetime_ticks=1;
  auto ch=emitter(*capped,d),fh=emitter(*full,d);
  CHECK(capped->submit(event(*capped,ch,1,7)));CHECK(full->submit(event(*full,fh,1,7)));
  CHECK(capped->step(1).value().dropped_particles==5);CHECK(full->step(1));
  CHECK(capped->submit(event(*capped,ch,2,1)));CHECK(full->submit(event(*full,fh,2,1)));
  CHECK(capped->step(2));CHECK(full->step(2));
  equal(capped->particles()[0],full->particles()[0],false);
}
void limits_and_generation() {
  Config c;c.max_emitters=1;c.max_particles=2;c.max_events=1;c.max_event_particles=5;
  auto s=system(c);auto h=emitter(*s);
  CHECK_ERROR(s->create_emitter(description(2)),ErrorCode::Exhausted);
  CHECK_ERROR(s->create_emitter(description()),ErrorCode::Duplicate);
  CHECK(s->submit(event(*s,h,1,4)).value().admission==Admission::Queued);
  CHECK(s->submit(event(*s,h,2,3)).value().admission==Admission::Dropped);
  CHECK_ERROR(s->submit(event(*s,h,2)),ErrorCode::InvalidInput);
  auto step=s->step(1);CHECK(step);CHECK(step.value().spawned==2);CHECK(step.value().dropped_particles==2);
  CHECK(s->statistics().dropped_particles==5);CHECK(s->statistics().dropped_events==1);
  CHECK(s->submit(event(*s,h,3,2)));CHECK(s->remove_emitter(h));
  CHECK(s->statistics().cancelled_events==1);CHECK(s->particles().size()==2);
  auto newer=emitter(*s);CHECK(newer.slot==h.slot);CHECK(newer.generation==h.generation+1);
  CHECK_ERROR(s->remove_emitter(h),ErrorCode::StaleHandle);
  CHECK_ERROR(s->submit(event(*s,h,4)),ErrorCode::StaleHandle);
  auto foreign=system(c);auto foreign_handle=emitter(*foreign);
  CHECK_ERROR(s->remove_emitter(foreign_handle),ErrorCode::StaleHandle);
  for(std::uint64_t tick=2;tick<=5;++tick)CHECK(s->step(tick));
  CHECK(s->particles().empty());CHECK(s->bounds().empty);
  CHECK(s->statistics().expired_particles==2);
  c.overload=OverloadPolicy::CoalesceMatching;auto coalesced=system(c);auto ce=emitter(*coalesced);
  CHECK(coalesced->submit(event(*coalesced,ce,1,3)));
  auto merged=coalesced->submit(event(*coalesced,ce,2,4));CHECK(merged);
  CHECK(merged.value().admission==Admission::Coalesced);
  CHECK(merged.value().accepted_particles==2&&merged.value().dropped_particles==2);
  auto differing=event(*coalesced,ce,3,1);differing.payload=Weather{{1,0,0}};
  CHECK(coalesced->submit(differing).value().admission==Admission::Dropped);
  CHECK(coalesced->step(1).value().processed_events==1);
  CHECK(coalesced->statistics().coalesced_events==1);
}
void invalid_and_overflow() {
  Config c;c.fixed_step_seconds=0;CHECK_ERROR(ParticleSystem::create(c),ErrorCode::InvalidInput);
  c={};c.max_events=0;CHECK_ERROR(ParticleSystem::create(c),ErrorCode::InvalidInput);
  c={};c.overload=static_cast<OverloadPolicy>(77);CHECK_ERROR(ParticleSystem::create(c),ErrorCode::InvalidInput);
  auto s=system();auto bad=description();bad.velocity.x=std::numeric_limits<double>::quiet_NaN();
  CHECK_ERROR(s->create_emitter(bad),ErrorCode::InvalidInput);
  bad=description();bad.lifetime_ticks=0;CHECK_ERROR(s->create_emitter(bad),ErrorCode::InvalidInput);
  bad=description();bad.velocity_spread.x=-1;CHECK_ERROR(s->create_emitter(bad),ErrorCode::InvalidInput);
  auto h=emitter(*s);auto e=event(*s,h);
  e.position.x=std::numeric_limits<double>::infinity();CHECK_ERROR(s->submit(e),ErrorCode::InvalidInput);
  e=event(*s,h);e.orientation.w=2;CHECK_ERROR(s->submit(e),ErrorCode::InvalidInput);
  e=event(*s,h);e.particles=0;CHECK_ERROR(s->submit(e),ErrorCode::InvalidInput);
  e=event(*s,h);e.effect_revision=2;CHECK_ERROR(s->submit(e),ErrorCode::StaleHandle);
  e=event(*s,h);e.world={9,9};CHECK_ERROR(s->submit(e),ErrorCode::StaleHandle);
  e=event(*s,h);e.payload=Splash{{0,std::numeric_limits<double>::quiet_NaN(),0},{}};
  CHECK_ERROR(s->submit(e),ErrorCode::InvalidInput);
  CHECK(s->submit(event(*s,h))); // Failed validation did not consume serial 1.
  CHECK_ERROR(s->step(2),ErrorCode::InvalidInput);CHECK(s->next_tick()==1);CHECK(s->particles().empty());
  CHECK(s->step(1));
  c={};c.first_tick=std::numeric_limits<std::uint64_t>::max();auto end=system(c);
  auto endh=emitter(*end);CHECK(end->submit(event(*end,endh)));
  CHECK_ERROR(end->step(c.first_tick),ErrorCode::Overflow);
  CHECK(end->next_tick()==c.first_tick);CHECK(end->particles().empty());
}
void integration_rebase_bounds() {
  Config c;c.fixed_step_seconds=0.25;auto a=system(c),b=system(c);
  auto d=description();d.position={3,2,1};d.velocity={2,3,-1};d.acceleration={0,-4,0};
  d.velocity_spread={};d.particles_per_tick=1;d.radius=0.1;d.lifetime_ticks=8;
  auto ah=emitter(*a,d),bh=emitter(*b,d);CHECK(a->step(1));CHECK(b->step(1));
  CHECK(a->particles()[0].position==d.position);CHECK(a->step(2));CHECK(b->step(2));
  CHECK(near(a->particles()[0].position,Vec3{3.5,2.625,0.75}));
  auto ae=event(*a,ah,1,1),be=event(*b,bh,1,1);ae.position=be.position=Vec3{7,8,9};
  CHECK(a->submit(ae));CHECK(b->submit(be));
  const Vec3 origin{1000,-2000,3000};CHECK(b->rebase(origin,2));
  CHECK(b->origin_revision()==2);CHECK(b->origin()==origin);
  auto stale=event(*b,bh,2);stale.origin_revision=1;
  CHECK_ERROR(b->submit(stale),ErrorCode::StaleHandle);
  CHECK_ERROR(b->rebase(origin,2),ErrorCode::InvalidInput);
  CHECK_ERROR(b->rebase({std::numeric_limits<double>::infinity(),0,0},3),ErrorCode::InvalidInput);
  for(std::uint64_t tick=3;tick<=6;++tick) {
    CHECK(a->step(tick));CHECK(b->step(tick));CHECK(a->particles().size()==b->particles().size());
    const auto bounds=b->bounds();CHECK(!bounds.empty);
    for(std::size_t i=0;i<a->particles().size();++i) {
      const auto x=a->particles()[i],y=b->particles()[i];
      CHECK(near(x.position,{y.position.x+origin.x,y.position.y+origin.y,y.position.z+origin.z}));
      CHECK(x.velocity==y.velocity);CHECK(x.id==y.id);CHECK(x.birth_tick==y.birth_tick);
      CHECK(y.origin_revision==2);CHECK(x.effect_revision==y.effect_revision);
      CHECK(bounds.minimum.x<=y.position.x-y.radius&&bounds.maximum.x>=y.position.x+y.radius);
      CHECK(bounds.minimum.y<=y.position.y-y.radius&&bounds.maximum.y>=y.position.y+y.radius);
      CHECK(bounds.minimum.z<=y.position.z-y.radius&&bounds.maximum.z>=y.position.z+y.radius);
    }
  }
  // Rotated event-local velocity; typed payload remains a copied producer value.
  auto rotated=system(c);d.particles_per_tick=0;d.velocity={1,0,0};d.acceleration={};
  auto rh=emitter(*rotated,d);auto r=event(*rotated,rh,1,1);r.payload=GameplayBurst{};
  r.orientation={0,0,std::sqrt(0.5),std::sqrt(0.5)};CHECK(rotated->submit(r));CHECK(rotated->step(1));
  CHECK(near(rotated->particles()[0].velocity,{0,1,0}));
}
}
int main() {
  deterministic_replay();independent_streams();limits_and_generation();
  invalid_and_overflow();integration_rebase_bounds();
  std::puts("vfx: deterministic replay, independent streams, bounded overload, stale handles, fixed-step integration, rebase and bounds passed");
}
