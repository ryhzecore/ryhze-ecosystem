#pragma once
#include "race/physics/owned_values.hpp"
#include <algorithm>
#include <cmath>
#include <span>
#include <type_traits>

namespace qualification::owned {
using namespace race::physics;
// Adapter-owned storage under qualification, not an application service. Product
// validation and copied-byte/decoded-descriptor agreement remain composition
// preconditions until the Assets boundary is connected and qualified.
using ProductBytes=std::array<std::byte,312>;
struct BodyConfiguration {
  std::uint64_t revision{};
  double mass_kg{}, friction{}, restitution{}, gravity_factor{1};
  std::uint32_t category{}, mask{};
  MotionType motion{MotionType::Static};
  bool sensor{}, allow_sleep{true};
};
struct BodyRecord {
  BodyIdentity identity{};
  ProductBytes product{};
  BodyConfiguration configuration{};
  WorldPose pose{};
  WorldMotion motion{};
  Primitive primitive{};
  std::uint32_t adapter_id{};
  bool occupied{};
};
enum class CommandKind : std::uint32_t { Create, Replace, Remove, Pose, Velocity, Impulse };
struct PendingCommand {
  BodyIdentity identity{};
  ProductBytes product{};
  BodyConfiguration configuration{};
  WorldPose pose{};
  WorldMotion motion{};
  Triple linear_impulse{}, angular_impulse{};
  std::uint64_t sequence{};
  CommandKind kind{CommandKind::Remove};
  bool has_product{};
};
struct IdentityRecord {
  BodyRevision body{};
  std::uint32_t adapter_id{};
  bool occupied{}, sensor{};
};
struct QuerySnapshotValue {
  BodyRevision body{};
  Primitive primitive{};
  WorldPose pose{};
  std::uint32_t category{}, mask{};
  bool occupied{}, sensor{};
};
struct PhaseValues {
  ResultStamp stamp{};
  std::array<PoseValue,PrimitiveProfile::bodies> poses{};
  std::array<ContactValue,PrimitiveProfile::events> events{};
  std::uint32_t pose_count{}, event_count{};
  bool failed{};

  bool append(ContactValue value) noexcept {
    if(failed)return false;
    const auto finite=[](const Triple& v){return std::all_of(v.begin(),v.end(),[](double x){return std::isfinite(x);});};
    const auto flags=static_cast<std::uint32_t>(value.validity);
    const bool exit=value.kind==EventKind::ContactExit||value.kind==EventKind::TriggerExit;
    if(static_cast<std::uint32_t>(value.kind)>static_cast<std::uint32_t>(EventKind::TriggerExit)||
       flags>7||(exit&&flags!=0)||!finite(value.point_first_m)||!finite(value.point_second_m)||
       !finite(value.normal_first_to_second)||!finite(value.relative_pre_solve_velocity_m_s)||
       !std::isfinite(value.penetration_m)||!std::isfinite(value.friction)||!std::isfinite(value.restitution)){
      failed=true;return false;
    }
    if(event_count==events.size()){failed=true;return false;}
    events[event_count++]=value;return true;
  }
  // Only call after SDK callback quiescence and before any reader publication.
  // In-place, bounded sort; no allocation or callback through the SDK.
  void order_events() noexcept {
    std::sort(events.begin(),events.begin()+event_count,[](const auto& a,const auto& b){
      const auto key=[](const BodyIdentity& v){return std::array{v.section.high,v.section.low,v.object.high,v.object.low,v.incarnation};};
      if(key(a.first.identity)!=key(b.first.identity))return key(a.first.identity)<key(b.first.identity);
      if(key(a.second.identity)!=key(b.second.identity))return key(a.second.identity)<key(b.second.identity);
      if(a.kind!=b.kind)return a.kind<b.kind;
      if(a.point_first_m!=b.point_first_m)return a.point_first_m<b.point_first_m;
      if(a.point_second_m!=b.point_second_m)return a.point_second_m<b.point_second_m;
      if(a.normal_first_to_second!=b.normal_first_to_second)return a.normal_first_to_second<b.normal_first_to_second;
      if(a.relative_pre_solve_velocity_m_s!=b.relative_pre_solve_velocity_m_s)return a.relative_pre_solve_velocity_m_s<b.relative_pre_solve_velocity_m_s;
      if(a.penetration_m!=b.penetration_m)return a.penetration_m<b.penetration_m;
      if(a.friction!=b.friction)return a.friction<b.friction;
      if(a.restitution!=b.restitution)return a.restitution<b.restitution;
      if(a.validity!=b.validity)return a.validity<b.validity;
      if(a.first.collision!=b.first.collision)return a.first.collision<b.first.collision;
      if(a.second.collision!=b.second.collision)return a.second.collision<b.second.collision;
      if(a.first.configuration!=b.first.configuration)return a.first.configuration<b.first.configuration;
      return a.second.configuration<b.second.configuration;
    });
  }
};
struct QueryValues {
  ResultStamp stamp{};
  std::array<QueryValue,PrimitiveProfile::hits_per_query> hits{};
  std::uint32_t count{};
};
class RemovalIdentities {
  std::array<IdentityRecord,PrimitiveProfile::bodies> current_{}, previous_{};
  std::array<bool,PrimitiveProfile::bodies> assigned_{};
public:
  // Snapshot exactly once at an accepted tick, before all retirements. This
  // value copy keeps old identities without retaining a body, shape or product.
  void begin_tick() noexcept {previous_=current_;assigned_.fill(false);}
  bool install(std::size_t index,IdentityRecord value) noexcept {
    if(index>=current_.size()||current_[index].occupied||assigned_[index]||!value.occupied||!value.body.identity.incarnation||
       !value.body.identity.object.valid()||!value.body.identity.section.valid())return false;
    for(std::size_t i=0;i<current_.size();++i)
      if(i!=index&&current_[i].occupied&&current_[i].adapter_id==value.adapter_id)return false;
    current_[index]=value;assigned_[index]=true;return true;
  }
  bool retire(std::size_t index) noexcept {
    if(index>=current_.size()||!current_[index].occupied)return false;
    current_[index]={};return true;
  }
  // SDK removal callbacks cannot inspect bodies. Prefer the predecessor, even
  // when its index/sequence has wrapped to a newly installed identity.
  const IdentityRecord* removed(std::uint32_t adapter_id) const noexcept {
    for(const auto& value:previous_)if(value.occupied&&value.adapter_id==adapter_id)return &value;
    for(const auto& value:current_)if(value.occupied&&value.adapter_id==adapter_id)return &value;
    return nullptr;
  }
  const IdentityRecord* current(std::uint32_t adapter_id) const noexcept {
    for(const auto& value:current_)if(value.occupied&&value.adapter_id==adapter_id)return &value;
    return nullptr;
  }
};
struct Workspace {
  std::array<BodyRecord,PrimitiveProfile::bodies> bodies{};
  std::array<PendingCommand,PrimitiveProfile::commands> pending{};
  RemovalIdentities identities{};
  std::array<QuerySnapshotValue,2*PrimitiveProfile::bodies> snapshots{};
  PhaseValues phase{};
  std::array<QueryValues,PrimitiveProfile::queries> queries{};
  std::array<std::byte,2*(PrimitiveProfile::bodies*(PrimitiveProfile::bodies-1)/2)> pairs{};
  std::array<std::byte,8> operand_alignment{};
  alignas(16) std::array<std::byte,512> temporary_query_operand{};
};
static_assert(sizeof(BodyRecord)<=576&&sizeof(PendingCommand)<=640);
static_assert(sizeof(IdentityRecord)<=128&&sizeof(QuerySnapshotValue)<=192);
static_assert(sizeof(PoseValue)<=192&&sizeof(ContactValue)<=288&&sizeof(QueryValue)<=144);
static_assert(sizeof(ResultStamp)+16<=256);
static_assert(sizeof(PhaseValues)<=256*192+4096*288+4096);
static_assert(sizeof(QueryValues)<=256*144+256);
static_assert(sizeof(Workspace)+131072+1048576<=6*1024*1024);
static_assert(std::is_trivially_copyable_v<BodyRecord>&&std::is_trivially_copyable_v<PendingCommand>);
static_assert(std::is_trivially_copyable_v<PhaseValues>&&std::is_trivially_copyable_v<QueryValues>);
} // namespace qualification::owned
