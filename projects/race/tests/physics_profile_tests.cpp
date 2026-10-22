#include "race/physics/profile.hpp"
#include "test.hpp"
#include <cmath>
#include <limits>
using namespace race;
using namespace race::physics;
int main() {
  const Primitive box{PrimitiveKind::Box,{2,4,6}},sphere{PrimitiveKind::Sphere,{2,2,2}};
  const auto scaled=bake_primitive(box,{2,.5,1},MotionType::Dynamic);
  CHECK(scaled&&scaled.value().dimensions_m==Triple{4,2,6});
  CHECK(bake_primitive({PrimitiveKind::Box,{100,100,100}},{1,1,1},MotionType::Static));
  CHECK_ERROR(bake_primitive({PrimitiveKind::Box,{100,1,1}},{1,1,1},MotionType::Kinematic),ErrorCode::InvalidInput);
  CHECK(bake_primitive({PrimitiveKind::Sphere,{.1,.1,.1}},{1,1,1},MotionType::Dynamic));
  CHECK_ERROR(bake_primitive(sphere,{1,2,1},MotionType::Dynamic),ErrorCode::InvalidInput);
  CHECK_ERROR(bake_primitive({static_cast<PrimitiveKind>(3),{1,1,1}},{1,1,1},MotionType::Static),ErrorCode::Unsupported);
  CHECK_ERROR(bake_primitive(box,{1,1,1},static_cast<MotionType>(0)),ErrorCode::Unsupported);
  for(const double bad:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
    CHECK_ERROR(bake_primitive(box,{bad,1,1},MotionType::Dynamic),ErrorCode::InvalidInput);
    CHECK_ERROR(primitive_mass(box,bad),ErrorCode::InvalidInput);
    CHECK_ERROR(bake_primitive({PrimitiveKind::Box,{bad,1,1}},{1,1,1},MotionType::Dynamic),ErrorCode::InvalidInput);
  }
  CHECK_ERROR(bake_primitive(box,{std::numeric_limits<double>::max(),1,1},MotionType::Static),ErrorCode::InvalidInput);
  // Analytic solid-shape inertias; dimensions are full lengths, never half extents.
  const auto mass=primitive_mass(box,12);CHECK(mass);
  CHECK(mass.value().inertia_kg_m2==Triple{52,40,20});
  const auto ball=primitive_mass(sphere,5);CHECK(ball);
  for(double value:ball.value().inertia_kg_m2)CHECK(std::abs(value-2)<1e-12);
  CHECK_ERROR(primitive_mass(box,100.1),ErrorCode::InvalidInput);
  CHECK_ERROR(primitive_mass(box,.09),ErrorCode::InvalidInput);
  MotionBound moving;moving.velocity_m_s={1.4,0,0};CHECK(admit_motion(sphere,moving));
  moving.velocity_m_s={1.6,0,0};CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  // Rational dt would admit this; the actual float solver dt exceeds 0.025 m.
  moving.velocity_m_s={1.49999996,0,0};
  CHECK(moving.velocity_m_s[0]*PrimitiveProfile::tick_seconds<.025);
  CHECK(moving.velocity_m_s[0]*static_cast<double>(PrimitiveProfile::solver_tick_seconds)>.025);
  CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  moving={};moving.acceleration_m_s2={0,-9.80665,0};const auto falling=admit_motion(sphere,moving);
  CHECK(falling&&falling.value()>=9.80665/(60.0*60.0)); // Semi-implicit bound, not half a*g*dt^2.
  moving.velocity_m_s={0,-1.4,0};CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  moving={};moving.angular_velocity_rad_s={0,2,0};CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  moving={};moving.angular_acceleration_rad_s2={0,100,0};CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  moving={};moving.velocity_m_s[2]=std::numeric_limits<double>::quiet_NaN();CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  moving={};moving.acceleration_m_s2[0]=std::numeric_limits<double>::max();CHECK_ERROR(admit_motion(sphere,moving),ErrorCode::InvalidInput);
  CapacityUse full{256,128,256,256,4096,64,256};CHECK(admit_capacity(full));
  const auto reject=[&](CapacityUse use){CHECK_ERROR(admit_capacity(use),ErrorCode::Exhausted);};
  auto use=full;++use.bodies;reject(use);use=full;++use.moving_bodies;reject(use);
  use=full;++use.shapes;reject(use);use=full;++use.commands;reject(use);
  use=full;++use.events;reject(use);use=full;++use.queries;reject(use);use=full;++use.hits;reject(use);
  CHECK_ERROR(admit_capacity({1,2}),ErrorCode::InvalidInput);
  std::puts("physics primitive admission: passed; no solver execution or allocation proof");
}
