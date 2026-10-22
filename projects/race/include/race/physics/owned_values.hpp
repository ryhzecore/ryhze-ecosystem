#pragma once
#include "race/foundation/digest.hpp"
#include "race/foundation/persistent_id.hpp"
#include "race/physics/profile.hpp"
#include <array>
#include <cstdint>

namespace race::physics {
// Portable values for the approved primitive profile. No SDK, Scene or Assets
// owner is hidden in these records. The composition boundary supplies IDs and
// validated revisions; these types alone do not admit a solver session.
struct BodyIdentity {
  PersistentId section{}, object{};
  std::uint64_t incarnation{};
  friend bool operator==(const BodyIdentity&,const BodyIdentity&)=default;
};
struct BodyRevision {
  BodyIdentity identity{};
  Digest256 collision{};
  std::uint64_t configuration{};
  friend bool operator==(const BodyRevision&,const BodyRevision&)=default;
};
struct WorldPose {
  Triple position_m{};
  // Scene convention: x, y, z, w. This is an owned rigid world pose; scale is
  // already baked into the independently validated collision product.
  std::array<double,4> rotation_xyzw{0,0,0,1};
};
struct WorldMotion { Triple linear_m_s{}, angular_rad_s{}; };
struct ResultStamp {
  PersistentId world{}, session{};
  std::uint64_t tick{}, input_generation{};
  Digest256 producer{}, products{}, configuration{};
  PersistentId origin{};
  std::uint64_t origin_revision{}, frame{};
  Triple origin_position_m{};
  friend bool operator==(const ResultStamp&,const ResultStamp&)=default;
};
struct PoseValue {
  BodyRevision body{};
  WorldPose pose{};
  WorldMotion motion{};
  std::uint64_t validity{};
};
enum class EventKind : std::uint32_t {
  ContactEnter, ContactPersist, ContactExit, TriggerEnter, TriggerPersist, TriggerExit
};
enum class ContactValidity : std::uint32_t {
  None=0, Geometry=1, PreSolveVelocity=2, Material=4
};
struct ContactValue {
  BodyRevision first{}, second{};
  Triple point_first_m{}, point_second_m{}, normal_first_to_second{};
  double penetration_m{};
  Triple relative_pre_solve_velocity_m_s{};
  double friction{}, restitution{};
  EventKind kind{EventKind::ContactExit};
  ContactValidity validity{ContactValidity::None};
  // There is intentionally no solved-impulse field: these callback values are
  // captured before solving. Exit geometry must be marked unavailable.
};
enum class QueryKind : std::uint32_t { Ray, PointOverlap, ShapeOverlap };
enum class QueryValidity : std::uint32_t { None=0, Point=1, Normal=2, Distance=4 };
struct QueryValue {
  BodyRevision body{};
  Triple point_m{}, normal{};
  double distance_m{};
  QueryKind kind{QueryKind::Ray};
  QueryValidity validity{QueryValidity::None};
};
} // namespace race::physics
