#include "owned_records.hpp"
#include "probe_composition.hpp"
#include "race/assets/collision.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
using namespace qualification::owned;
namespace collision=race::assets::collision;
void check(bool passed,const char* reason) noexcept {
  if(!passed){std::fprintf(stderr,"RACE_PRODUCT_OWNER_REJECT: %s\n",reason);std::abort();}
}
// Executable-composition qualification: Assets validates its own wire format;
// Physics receives fixed copied values. Neither portable domain includes its
// sibling, and no duplicate collision parser is introduced here.
race::Result<PendingCommand> stage(std::span<const std::byte> borrowed,race::assets::AssetId id,
    race::Digest256 revision,BodyIdentity identity,BodyConfiguration configuration,CommandKind kind){
  if(borrowed.size()!=collision::product_bytes||!id.valid()||revision==race::Digest256{}||
      !identity.section.valid()||!identity.object.valid()||!identity.incarnation||!configuration.revision||
      (kind!=CommandKind::Create&&kind!=CommandKind::Replace))
    return race::fail(race::ErrorCode::InvalidInput,"invalid owned product admission identity or size");
  static_assert(std::tuple_size_v<ProductBytes> == collision::product_bytes);
  PendingCommand result;result.identity=identity;result.configuration=configuration;result.kind=kind;
  // Copy first, then validate precisely the retained bytes. View never escapes
  // this scope and does not refer to the caller's buffer during verification.
  std::copy(borrowed.begin(),borrowed.end(),result.product.begin());
  auto view=collision::validate(result.product);if(!view)return view.error();
  if(view.value().id()!=id||view.value().revision()!=revision)
    return race::fail(race::ErrorCode::StaleHandle,"collision identity or pinned revision differs");
  const auto& primitive=view.value().primitive();
  Primitive geometry{primitive.kind==collision::Kind::Sphere?PrimitiveKind::Sphere:PrimitiveKind::Box,primitive.dimensions_m};
  auto admitted=bake_primitive(geometry,{1,1,1},configuration.motion);if(!admitted)return admitted.error();
  if(configuration.motion==MotionType::Dynamic){
    auto mass=primitive_mass(admitted.value(),configuration.mass_kg);if(!mass)return mass.error();
  }
  result.has_product=true;return result;
}
race::Result<BodyRecord> admit_active(const PendingCommand& pending,race::assets::AssetId id,race::Digest256 revision){
  if(!pending.has_product)return race::fail(race::ErrorCode::InvalidInput,"pending command has no owned collision product");
  auto checked=stage(pending.product,id,revision,pending.identity,pending.configuration,pending.kind);
  if(!checked)return checked.error();
  auto view=collision::validate(checked.value().product);if(!view)return view.error();
  BodyRecord body;body.identity=checked.value().identity;body.configuration=checked.value().configuration;
  // Pose/motion and the remaining body settings require their separate batch
  // admission; product validation must preserve, not replace, their owned values.
  body.product=checked.value().product;body.pose=pending.pose;body.motion=pending.motion;
  body.primitive={view.value().primitive().kind==collision::Kind::Sphere?PrimitiveKind::Sphere:PrimitiveKind::Box,view.value().primitive().dimensions_m};
  body.occupied=true;return body;
}
} // namespace

bool qualification::composition::prepare_products(Control& control) noexcept {
  // Fixed valid fixture inputs exercise the real codec and product owner. This
  // helper is not the eventual arbitrary command-batch admission interface.
  try {
    collision::Stamp stamp;stamp.representation=collision::Representation::Cooked;stamp.target=collision::portable_cpu_target;
    stamp.source=race::assets::digest(std::as_bytes(std::span("composed source",15)));
    stamp.importer=stamp.settings=stamp.cooker=stamp.source;
    for(std::size_t i=0;i<control.bodies.size();++i){
      const race::assets::AssetId id{1,i+1};
      auto bytes=collision::encode(id,stamp,{collision::Kind::Box,{1,1,1}});if(!bytes)return false;
      BodyConfiguration configuration{};configuration.revision=1;configuration.mass_kg=1;configuration.friction=.2;
      configuration.category=configuration.mask=1;configuration.sensor=i==129;configuration.allow_sleep=false;
      configuration.motion=i<128?MotionType::Dynamic:MotionType::Static;
      auto pending=stage(bytes.value(),id,race::assets::digest(bytes.value()),{{1,1},{1,i+1},1},configuration,CommandKind::Create);
      if(!pending)return false;
      pending.value().pose.position_m={double(i%16)*2,i<2?1.:(i<128?20.:0.),double((i%128)/16)*2};
      control.pending[i]=pending.value();
      auto active=admit_active(control.pending[i],id,race::assets::digest(bytes.value()));if(!active)return false;
      control.bodies[i]=active.value();
    }
    return true;
  }catch(const std::bad_alloc&){return false;}
}

void run_product_owner_probe(){
  collision::Stamp stamp;stamp.representation=collision::Representation::Cooked;stamp.target=collision::portable_cpu_target;
  stamp.source.fill(std::byte{1});stamp.settings.fill(std::byte{2});stamp.importer.fill(std::byte{3});stamp.cooker.fill(std::byte{4});
  const race::assets::AssetId id{1,2};const BodyIdentity body_id{{3,4},{5,6},7};
  BodyConfiguration configuration;configuration.revision=8;configuration.mass_kg=1;configuration.motion=MotionType::Dynamic;
  auto encoded=collision::encode(id,stamp,{collision::Kind::Box,{1,2,3}});check(bool(encoded),"canonical collision encoding");
  const auto revision=race::assets::digest(encoded.value());const auto original=encoded.value();
  auto pending=stage(encoded.value(),id,revision,body_id,configuration,CommandKind::Create);
  check(bool(pending)&&pending.value().has_product,"admit copied validated product");
  encoded.value().fill(std::byte{});
  check(pending.value().product==original,"caller buffer can be released or reused");
  pending.value().pose.position_m={10,20,30};pending.value().motion.linear_m_s={.1,.2,.3};
  auto active=admit_active(pending.value(),id,revision);check(bool(active),"active owner revalidates exact staged bytes");
  pending.value().product.fill(std::byte{});
  check(active.value().product==original&&active.value().identity==body_id&&active.value().configuration.revision==8,
        "active body owns bytes after pending command retirement");
  check(active.value().primitive.kind==PrimitiveKind::Box&&active.value().primitive.dimensions_m==Triple{1,2,3},"decoded descriptor matches owned product");
  check(active.value().pose.position_m==Triple{10,20,30}&&active.value().motion.linear_m_s==Triple{.1,.2,.3},"product admission preserves pending pose and motion");
  auto old=collision::validate(active.value().product);
  check(bool(old)&&old.value().revision()==revision&&old.value().stamp()==stamp,"active provenance and revision retained");
  std::size_t rejections{};
  const auto reject=[&](auto value){check(!value,"invalid product admission must fail");++rejections;check(active.value().product==original,"failed preflight preserves active bytes");};
  auto missing_product=pending.value();missing_product.product=original;missing_product.has_product=false;
  reject(admit_active(missing_product,id,revision));
  reject(stage(std::span(original).first(311),id,revision,body_id,configuration,CommandKind::Replace));
  std::array<std::byte,313> overlong{};reject(stage(overlong,id,revision,body_id,configuration,CommandKind::Replace));
  auto corrupted=original;corrupted[256]^=std::byte{1};
  reject(stage(corrupted,id,revision,body_id,configuration,CommandKind::Replace));
  reject(stage(original,{9,9},revision,body_id,configuration,CommandKind::Replace));
  auto wrong_revision=revision;wrong_revision[0]^=std::byte{1};
  reject(stage(original,id,wrong_revision,body_id,configuration,CommandKind::Replace));
  auto missing_incarnation=body_id;missing_incarnation.incarnation=0;
  reject(stage(original,id,revision,missing_incarnation,configuration,CommandKind::Replace));
  auto missing_configuration=configuration;missing_configuration.revision=0;
  reject(stage(original,id,revision,body_id,missing_configuration,CommandKind::Replace));
  reject(stage(original,id,revision,body_id,configuration,CommandKind::Remove));
  auto bad_mass=configuration;bad_mass.mass_kg=101;
  reject(stage(original,id,revision,body_id,bad_mass,CommandKind::Replace));
  auto large=collision::encode(id,stamp,{collision::Kind::Box,{20,20,20}});check(bool(large),"valid static-only collision product");
  reject(stage(large.value(),id,race::assets::digest(large.value()),body_id,configuration,CommandKind::Replace));
  auto static_configuration=configuration;static_configuration.motion=MotionType::Static;
  check(bool(stage(large.value(),id,race::assets::digest(large.value()),body_id,static_configuration,CommandKind::Replace)),"static dimensions preserve approved wider profile");
  auto next=collision::encode(id,stamp,{collision::Kind::Sphere,{2,2,2}});check(bool(next),"replacement sphere encoding");
  auto replacement_configuration=configuration;++replacement_configuration.revision;
  auto replacement_id=body_id;++replacement_id.incarnation;
  auto replacement=stage(next.value(),id,race::assets::digest(next.value()),replacement_id,replacement_configuration,CommandKind::Replace);
  check(bool(replacement)&&active.value().product==original,"hot reload stages independent owned replacement");
  auto candidate=admit_active(replacement.value(),id,race::assets::digest(next.value()));
  check(bool(candidate)&&candidate.value().primitive.kind==PrimitiveKind::Sphere&&candidate.value().identity.incarnation==8&&
        candidate.value().configuration.revision==9&&active.value().product==original,"candidate and previous product coexist without mutation");
  std::printf("{\"product_owner_passed\":true,\"product_bytes\":312,\"rejections\":%zu,\"codec_reused\":true,\"source_borrow_released\":true,\"pending_retired\":true,\"old_product_preserved\":true,\"whole_batch_qualified\":false,\"solver_executed\":false,\"allocation_proved\":false}\n",rejections);
}
