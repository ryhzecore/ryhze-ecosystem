#include "race/physics/profile.hpp"
#include <cmath>
#include <limits>

namespace race::physics {
namespace {
bool valid(MotionType type) noexcept {
  return type==MotionType::Static||type==MotionType::Kinematic||type==MotionType::Dynamic;
}
Result<void> validate(Primitive shape,MotionType motion) noexcept {
  if(!valid(motion)||(shape.kind!=PrimitiveKind::Sphere&&shape.kind!=PrimitiveKind::Box))
    return fail(ErrorCode::Unsupported,"primitive or motion kind is not admitted");
  const double maximum=shape.kind==PrimitiveKind::Box&&motion==MotionType::Static?100.0:10.0;
  for(const double dimension:shape.dimensions_m)
    if(!std::isfinite(dimension)||dimension<0.1||dimension>maximum)
      return fail(ErrorCode::InvalidInput,"primitive dimension outside the approved profile");
  if(shape.kind==PrimitiveKind::Sphere&&
     (shape.dimensions_m[0]!=shape.dimensions_m[1]||shape.dimensions_m[0]!=shape.dimensions_m[2]))
    return fail(ErrorCode::InvalidInput,"sphere dimensions must be uniform");
  return {};
}
// Round positive bounds outward. L1 bounds avoid relying on a particular libm's
// sqrt/hypot error when proving a conservative displacement envelope.
double up(double value) noexcept { return std::nextafter(value,std::numeric_limits<double>::infinity()); }
double magnitude_bound(Triple value) noexcept {
  double sum=0;
  for(double component:value) {
    if(!std::isfinite(component))return std::numeric_limits<double>::infinity();
    if(component!=0)sum=up(sum+std::abs(component));
  }
  return sum;
}
}
Result<Primitive> bake_primitive(Primitive source,Triple scale,MotionType motion) noexcept {
  if(source.kind!=PrimitiveKind::Sphere&&source.kind!=PrimitiveKind::Box)
    return fail(ErrorCode::Unsupported,"primitive kind is not admitted");
  if(source.kind==PrimitiveKind::Sphere&&
     (scale[0]!=scale[1]||scale[0]!=scale[2]||source.dimensions_m[0]!=source.dimensions_m[1]||source.dimensions_m[0]!=source.dimensions_m[2]))
    return fail(ErrorCode::InvalidInput,"sphere scale must be uniform");
  for(std::size_t axis=0;axis<3;++axis) {
    if(!std::isfinite(scale[axis])||scale[axis]<=0||!std::isfinite(source.dimensions_m[axis])||source.dimensions_m[axis]<=0)
      return fail(ErrorCode::InvalidInput,"primitive dimensions and scale must be finite and positive");
    source.dimensions_m[axis]*=scale[axis];
  }
  if(auto checked=validate(source,motion);!checked)return checked.error();
  return source;
}
Result<MassProperties> primitive_mass(Primitive shape,double mass) noexcept {
  if(auto checked=validate(shape,MotionType::Dynamic);!checked)return checked.error();
  if(!std::isfinite(mass)||mass<0.1||mass>100)
    return fail(ErrorCode::InvalidInput,"dynamic mass outside the approved profile");
  MassProperties result{mass,{}};
  if(shape.kind==PrimitiveKind::Sphere) {
    const double radius=shape.dimensions_m[0]*0.5;
    result.inertia_kg_m2.fill((2.0/5.0)*mass*radius*radius);
  } else {
    for(std::size_t axis=0;axis<3;++axis) {
      const double a=shape.dimensions_m[(axis+1)%3],b=shape.dimensions_m[(axis+2)%3];
      result.inertia_kg_m2[axis]=(mass/12.0)*(a*a+b*b);
    }
  }
  return result;
}
Result<double> admit_motion(Primitive shape,MotionBound motion) noexcept {
  if(auto checked=validate(shape,MotionType::Dynamic);!checked)return checked.error();
  // The sphere's radius is exact; half the box's L1 diagonal encloses all points.
  double radius=shape.dimensions_m[0]*0.5;
  if(shape.kind==PrimitiveKind::Box)radius=up(magnitude_bound(shape.dimensions_m)*0.5);
  const double dt=up(static_cast<double>(PrimitiveProfile::solver_tick_seconds));
  const double linear=up(up(magnitude_bound(motion.velocity_m_s)+up(magnitude_bound(motion.acceleration_m_s2)*dt))*dt);
  const double angular=up(up(magnitude_bound(motion.angular_velocity_rad_s)+up(magnitude_bound(motion.angular_acceleration_rad_s2)*dt))*dt);
  const double displacement=up(linear+up(radius*angular));
  if(!std::isfinite(displacement)||displacement>PrimitiveProfile::max_displacement_m)
    return fail(ErrorCode::InvalidInput,"discrete-motion bound exceeded; CCD profile required");
  return displacement;
}
Result<void> admit_capacity(CapacityUse use) noexcept {
  if(use.moving_bodies>use.bodies)
    return fail(ErrorCode::InvalidInput,"moving count exceeds total body count");
  if(use.bodies>PrimitiveProfile::bodies||use.moving_bodies>PrimitiveProfile::moving_bodies||
     use.shapes>PrimitiveProfile::shapes||use.commands>PrimitiveProfile::commands||
     use.events>PrimitiveProfile::events||use.queries>PrimitiveProfile::queries||use.hits>PrimitiveProfile::hits_per_query)
    return fail(ErrorCode::Exhausted,"primitive profile capacity exceeded");
  return {};
}
} // namespace race::physics
