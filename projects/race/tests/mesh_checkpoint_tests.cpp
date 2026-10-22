#include "mesh_checkpoint.hpp"
#include "mesh_reimport.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <algorithm>
#include <array>

namespace c=race::mesh_checkpoint;
namespace m=race::assets::mesh;
namespace {
constexpr std::string_view source="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view changed="v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n";
constexpr std::string_view uv="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
struct FaultFiles final:FakeFiles {
  bool corrupt{},oversize{};
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override{
    auto result=FakeFiles::read(leaf,limit);if(!result)return result.error();
    if(corrupt&&!result.value().empty())result.value().back()^=std::byte{1};
    if(oversize)result.value().reserve(limit+1);return result;
  }
};
std::uint64_t next_owner=1;
struct Fixture {
  std::shared_ptr<FaultFiles> products=std::make_shared<FaultFiles>();
  FaultFiles checkpoints;FakeFiles recipes;
  TrackedAllocator memory=TrackedAllocator::create({64*1024*1024}).value();
  std::optional<mesh_project::Snapshot> project;
  std::unique_ptr<mesh_import::Reimport> owner;
  std::vector<std::byte> imported,cooked;
  explicit Fixture(std::string_view text=source,mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry){
    auto recipe=mesh_recipe::encode({aid(1),"source.obj",profile}).value();recipes.data["recipe.json"]={bytes(recipe).begin(),bytes(recipe).end()};
    std::array<mesh_project::Reference,1> refs{{{aid(1),"recipe.json",digest(bytes(recipe))}}};auto index=mesh_project::encode(refs).value();
    auto admitted=mesh_project::Snapshot::open(std::span<const std::byte>(index),recipes);CHECK(admitted);project=std::move(admitted).value();
    imported=mesh_pipeline::import_obj(aid(1),bytes(text),profile).value();cooked=mesh_pipeline::cook(imported).value();
    CHECK(mesh_pipeline::publish(*products,cooked));auto made=mesh_import::Reimport::create(next_owner++,aid(1),products,memory,{},profile);CHECK(made);owner=std::move(made).value();CHECK(owner->restore_initial(cooked));
  }
  m::Lease lease(){return owner->acquire(owner->current().value()).value();}
  c::Published capture(){auto result=c::capture(*project,lease(),*products,checkpoints);CHECK(result);return std::move(result).value();}
  std::string manifest(c::Definition definition){auto text=c::encode(definition).value();auto leaf=hex(digest(bytes(text)))+".json";checkpoints.data[leaf]={bytes(text).begin(),bytes(text).end()};return leaf;}
};
void profiles(){
  using P=mesh_pipeline::Profile;
  for(auto profile:{P::Geometry,P::GeometryUv}){
    Fixture f(profile==P::GeometryUv?uv:source,profile);const auto saved=f.capture();auto loaded=c::load(f.checkpoints,saved.leaf,*f.project,*f.products);CHECK(loaded);
    auto view=m::validate(loaded.value().product());CHECK(view&&view.value().has_uv()==(profile==P::GeometryUv));
    std::array<m::Vertex,3> vertices;for(std::size_t i=0;i<vertices.size();++i)vertices[i]=view.value().vertex(i).value();
    const std::array<std::uint32_t,3> indices{0,1,2};
    const auto forged=m::encode(aid(1),view.value().stamp(),vertices,indices,{},profile!=P::GeometryUv).value();
    // Full revision points at these valid but wrong-schema bytes: schema cannot
    // be inferred from hashes, nor admitted solely because the stamp matches.
    auto bad=saved.definition;bad.cooked_revision=digest(forged);f.products->data[hex(bad.cooked_revision)+".rasset"]=forged;
    CHECK_ERROR(c::load(f.checkpoints,f.manifest(bad),*f.project,*f.products),ErrorCode::InvalidDependency);
    auto store=m::Store::create({},f.memory);CHECK(store);auto update=store.value()->begin_update(aid(1),view.value().stamp());CHECK(update);
    auto published=store.value()->publish(update.value(),forged);CHECK(published);auto lease=store.value()->acquire(published.value());CHECK(lease);
    const auto prior=f.checkpoints.data;CHECK_ERROR(c::capture(*f.project,lease.value(),*f.products,f.checkpoints),ErrorCode::InvalidDependency);CHECK(f.checkpoints.data==prior);
    // Recipe pin changes to the opposite explicit profile, while all outer
    // checkpoint/index/revision identities are recomputed consistently.
    const auto recipe=mesh_recipe::encode({aid(1),"source.obj",profile==P::GeometryUv?P::Geometry:P::GeometryUv}).value();
    f.recipes.data["other.json"]={bytes(recipe).begin(),bytes(recipe).end()};
    const std::array<mesh_project::Reference,1> refs{{{aid(1),"other.json",digest(bytes(recipe))}}};
    const auto index=mesh_project::encode(refs).value();auto other=mesh_project::Snapshot::open(std::span<const std::byte>(index),f.recipes);CHECK(other);
    auto crossed=saved.definition;crossed.index_revision=other.value().revision();crossed.recipe_revision=other.value().find(aid(1)).value()->recipe.revision;
    CHECK_ERROR(c::load(f.checkpoints,f.manifest(crossed),other.value(),*f.products),ErrorCode::InvalidDependency);
    CHECK_ERROR(c::capture(other.value(),f.lease(),*f.products,f.checkpoints),ErrorCode::InvalidDependency);
    CHECK(c::load(f.checkpoints,saved.leaf,*f.project,*f.products));
  }
}
void roundtrip(){
  Fixture f;auto saved=f.capture();auto old=f.checkpoints.data;auto repeated=f.capture();CHECK(saved.leaf==repeated.leaf&&f.checkpoints.data==old);
  CHECK(saved.definition.source==digest(bytes(source))&&saved.definition.cooked_revision==digest(f.cooked));
  auto loaded=c::load(f.checkpoints,saved.leaf,*f.project,*f.products);CHECK(loaded&&loaded.value().definition()==saved.definition);
  CHECK(std::equal(loaded.value().product().begin(),loaded.value().product().end(),f.cooked.begin(),f.cooked.end()));
  const auto raw=loaded.value().manifest();CHECK(c::decode(raw).value()==saved.definition);
  auto canonical=c::encode(saved.definition).value();auto exact=canonical;exact.resize(c::maximum_bytes,' ');CHECK(c::decode(bytes(exact)));exact+=' ';CHECK_ERROR(c::decode(bytes(exact)),ErrorCode::Exhausted);
  auto padded=canonical;padded.resize(c::maximum_bytes,' ');auto padded_leaf=hex(digest(bytes(padded)))+".json";
  f.checkpoints.data[padded_leaf]={bytes(padded).begin(),bytes(padded).end()};
  CHECK(c::load(f.checkpoints,padded_leaf,*f.project,*f.products));
  m::Limits limit;limit.product_bytes=f.cooked.size();
  CHECK(c::capture(*f.project,f.lease(),*f.products,f.checkpoints,limit));
  CHECK(c::load(f.checkpoints,saved.leaf,*f.project,*f.products,limit));
  --limit.product_bytes;CHECK(!c::capture(*f.project,f.lease(),*f.products,f.checkpoints,limit));
  CHECK(!c::load(f.checkpoints,saved.leaf,*f.project,*f.products,limit));
  auto moved=std::move(loaded).value();CHECK(loaded.value().manifest().empty()&&loaded.value().product().empty()&&!loaded.value().definition().id.valid()&&loaded.value().retained_bytes()==sizeof(c::Loaded));
  auto replacement=c::load(f.checkpoints,saved.leaf,*f.project,*f.products);CHECK(replacement);replacement.value()=std::move(moved);CHECK(moved.retained_bytes()==sizeof(c::Loaded)&&moved.revision()==Digest{});
  f.owner.reset();f.project.reset();f.products->data.clear();f.checkpoints.data.clear();
  CHECK(replacement.value().definition()==saved.definition&&digest(replacement.value().product())==saved.definition.cooked_revision);
}
void metadata(){
  Fixture f;auto saved=f.capture();auto text=c::encode(saved.definition).value();
  std::vector<std::string> fields;for(std::size_t first=1;first<text.size();){const auto end=text.find_first_of(",}",first);CHECK(end!=text.npos);fields.push_back(text.substr(first,end-first));if(text[end]=='}')break;first=end+1;}
  auto join=[](const std::vector<std::string>& items){std::string result="{";for(const auto& item:items){if(result.size()>1)result+=',';result+=item;}return result+'}';};
  auto reverse=fields;std::reverse(reverse.begin(),reverse.end());auto decoded=c::decode(bytes(join(reverse)));CHECK(decoded&&decoded.value()==saved.definition&&c::encode(decoded.value()).value()==text);
  for(std::size_t n=0;n<fields.size();++n){auto missing=fields;missing.erase(missing.begin()+static_cast<std::ptrdiff_t>(n));CHECK(!c::decode(bytes(join(missing))));auto duplicate=fields;duplicate[n]=fields[(n+1)%fields.size()];CHECK(!c::decode(bytes(join(duplicate))));}
  for(auto input:{"","{}","[]","null","{\"schema\":2}","{\"schema\":1,}"})CHECK(!c::decode(bytes(input)));
  for(auto version:{"0","2","01","-1","1e0","1.0","4294967296"}){auto bad=fields;bad[0]="\"schema\":"+std::string(version);CHECK(!c::decode(bytes(join(bad))));}
  auto malformed=text;malformed.push_back('\0');CHECK(!c::decode(bytes(malformed)));CHECK(!c::decode(bytes(text+"{}")));
  for(std::size_t n=1;n<fields.size();++n){auto bad=fields;bad[n][bad[n].find(':')+2]='G';CHECK(!c::decode(bytes(join(bad))));}
  auto bad=saved.definition;bad.id={};CHECK(!c::encode(bad));bad=saved.definition;bad.source={};CHECK(!c::encode(bad));
  CHECK(!c::load(f.checkpoints,"../saved.json",*f.project,*f.products));CHECK(!c::load(f.checkpoints,"saved.json",*f.project,*f.products));
  auto wrong_leaf=std::string(64,'a')+".json";f.checkpoints.data[wrong_leaf]=f.checkpoints.data[saved.leaf];CHECK(!c::load(f.checkpoints,wrong_leaf,*f.project,*f.products));
}
void closure(){
  Fixture f;auto saved=f.capture();const auto prior=f.checkpoints.data;const auto current=f.owner->current().value();
  for(unsigned n=0;n<5;++n){auto definition=saved.definition;
    if(n==0)definition.index_revision[0]^=std::byte{1};if(n==1)definition.recipe_revision[0]^=std::byte{1};
    if(n==2)definition.source[0]^=std::byte{1};if(n==3)definition.cooked_revision[0]^=std::byte{1};if(n==4)definition.id=aid(2);
    auto leaf=f.manifest(definition);CHECK(!c::load(f.checkpoints,leaf,*f.project,*f.products));CHECK(f.owner->current().value()==current);
  }
  auto imported=saved.definition;imported.cooked_revision=digest(f.imported);f.products->data[hex(imported.cooked_revision)+".rasset"]=f.imported;
  CHECK_ERROR(c::load(f.checkpoints,f.manifest(imported),*f.project,*f.products),ErrorCode::InvalidDependency);
  // A new desired source is not the source of the still-current cooked lease.
  CHECK(f.owner->desire(digest(bytes(changed))));auto captured=f.capture();CHECK(captured.definition.source==digest(bytes(source))&&captured.leaf==saved.leaf);
  f.products->corrupt=true;CHECK(!c::load(f.checkpoints,saved.leaf,*f.project,*f.products));CHECK(!c::capture(*f.project,f.lease(),*f.products,f.checkpoints));f.products->corrupt=false;
  auto removed=f.products->data.extract(hex(saved.definition.cooked_revision)+".rasset");CHECK(!removed.empty());CHECK_ERROR(c::load(f.checkpoints,saved.leaf,*f.project,*f.products),ErrorCode::NotFound);f.products->data.insert(std::move(removed));
  for(const auto& [leaf,raw]:prior)CHECK(f.checkpoints.data[leaf]==raw);
  m::Limits too_large;too_large.product_bytes=c::maximum_product_bytes+1;CHECK(!c::load(f.checkpoints,saved.leaf,*f.project,*f.products,too_large));
}
void preservation(){
  Fixture f;const auto saved=f.capture();const auto old=f.checkpoints.data;auto held=f.lease();
  CHECK_ERROR(c::capture(*f.project,m::Lease{nullptr},*f.products,f.checkpoints),ErrorCode::InvalidInput);
  f.checkpoints.fail_write=f.checkpoints.writes;CHECK(!c::capture(*f.project,held,*f.products,f.checkpoints));CHECK(f.checkpoints.data==old);f.checkpoints.fail_write=SIZE_MAX;
  f.checkpoints.corrupt=true;CHECK_ERROR(c::capture(*f.project,held,*f.products,f.checkpoints),ErrorCode::IoFailure);f.checkpoints.corrupt=false;CHECK(f.checkpoints.data==old);
  f.checkpoints.oversize=true;CHECK_ERROR(c::capture(*f.project,held,*f.products,f.checkpoints),ErrorCode::Exhausted);CHECK_ERROR(c::load(f.checkpoints,saved.leaf,*f.project,*f.products),ErrorCode::Exhausted);f.checkpoints.oversize=false;
  f.products->oversize=true;CHECK_ERROR(c::capture(*f.project,held,*f.products,f.checkpoints),ErrorCode::Exhausted);CHECK_ERROR(c::load(f.checkpoints,saved.leaf,*f.project,*f.products),ErrorCode::Exhausted);f.products->oversize=false;
  f.products->throw_read=true;CHECK_ERROR(c::capture(*f.project,held,*f.products,f.checkpoints),ErrorCode::Exhausted);f.products->throw_read=false;
  f.checkpoints.throw_read=true;CHECK_ERROR(c::load(f.checkpoints,saved.leaf,*f.project,*f.products),ErrorCode::Exhausted);f.checkpoints.throw_read=false;
  auto conflict=f.checkpoints.data;conflict[saved.leaf].back()^=std::byte{1};f.checkpoints.data=conflict;CHECK_ERROR(c::capture(*f.project,held,*f.products,f.checkpoints),ErrorCode::Duplicate);CHECK(f.checkpoints.data==conflict);
  f.checkpoints.data=old;CHECK(c::load(f.checkpoints,saved.leaf,*f.project,*f.products));f.owner.reset();CHECK(held->view().revision()==saved.definition.cooked_revision);
  // Readback can fail after a complete new immutable file becomes visible.
  Fixture newer(changed);newer.checkpoints.data=old;const auto next_current=newer.owner->current().value();
  const c::Definition next{aid(1),newer.project->revision(),newer.project->find(aid(1)).value()->recipe.revision,digest(bytes(changed)),digest(newer.cooked)};
  const auto next_text=c::encode(next).value(),next_leaf=hex(digest(bytes(next_text)))+".json";
  newer.checkpoints.corrupt=true;CHECK_ERROR(c::capture(*newer.project,newer.lease(),*newer.products,newer.checkpoints),ErrorCode::IoFailure);
  CHECK(newer.owner->current().value()==next_current&&newer.checkpoints.data.size()==old.size()+1);
  CHECK(digest(newer.checkpoints.data[next_leaf])==digest(bytes(next_text)));
  for(const auto& [name,raw]:old)CHECK(newer.checkpoints.data[name]==raw);
  newer.checkpoints.corrupt=false;CHECK(c::load(newer.checkpoints,next_leaf,*newer.project,*newer.products));
  // A lower admitted geometry limit must reject even a valid retained lease.
  std::string double_face(source);double_face+="f 1 2 3\n";Fixture larger(double_face);m::Limits limits;limits.vertices=3;
  CHECK(!c::capture(*larger.project,larger.lease(),*larger.products,larger.checkpoints,limits));CHECK(larger.checkpoints.data.empty());
  limits=m::Limits{};limits.product_bytes=larger.cooked.size();
  auto larger_saved=c::capture(*larger.project,larger.lease(),*larger.products,larger.checkpoints,limits);CHECK(larger_saved);
  CHECK(c::load(larger.checkpoints,larger_saved.value().leaf,*larger.project,*larger.products,limits));
  --limits.product_bytes;CHECK(m::validate_limits(limits));
  CHECK_ERROR(c::capture(*larger.project,larger.lease(),*larger.products,larger.checkpoints,limits),ErrorCode::Exhausted);
  CHECK_ERROR(c::load(larger.checkpoints,larger_saved.value().leaf,*larger.project,*larger.products,limits),ErrorCode::Exhausted);
  const auto larger_preserved=larger.checkpoints.data;
  limits=m::Limits{};limits.indices=3;
  CHECK(!c::capture(*larger.project,larger.lease(),*larger.products,larger.checkpoints,limits));CHECK(larger.checkpoints.data==larger_preserved);
}
void capture_lifetime(){
  Fixture f;auto held=f.lease();const auto expected=held->view().revision();
  f.products->gate=std::make_shared<Gate>();f.products->gated=hex(expected)+".rasset";
  std::optional<Result<c::Published>> result;
  std::thread reader([&,copy=held]() mutable{result.emplace(c::capture(*f.project,std::move(copy),*f.products,f.checkpoints));});
  f.products->gate->wait();f.owner.reset();held=m::Lease{nullptr};CHECK(f.memory.snapshot().total.live==f.cooked.size());
  f.products->gate->release();reader.join();CHECK(result&&*result&&result->value().definition.cooked_revision==expected);CHECK(f.memory.snapshot().total.live==0);
}
void allocations(){
  Fixture f;const auto saved=f.capture();const auto original=f.checkpoints.data;std::size_t failures{};
  for(std::size_t n=0;n<128;++n){
    desktop::profile::testing::fail_allocation_after(n);auto result=c::capture(*f.project,f.lease(),*f.products,f.checkpoints);desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;CHECK(result.error().code==ErrorCode::Exhausted&&f.checkpoints.data==original);++failures;
  }
  CHECK(failures>3&&failures<128);failures=0;
  for(std::size_t n=0;n<64;++n){
    desktop::profile::testing::fail_allocation_after(n);auto result=c::load(f.checkpoints,saved.leaf,*f.project,*f.products);desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;CHECK(result.error().code==ErrorCode::Exhausted&&f.checkpoints.data==original);++failures;
  }
  CHECK(failures>1&&failures<64);
}
void measured(){
  std::string dense="v 0 0 0\nv 1 0 0\nv 0 1 0\n";for(unsigned n=0;n<12000;++n)dense+="f 1 2 3\n";
  Fixture f(dense);const auto saved=f.capture();std::array<double,9> capture{},load{},restore{};std::size_t retained{};
  using Clock=std::chrono::steady_clock;auto elapsed=[](auto d){return std::chrono::duration<double,std::milli>(d).count();};
  for(unsigned n=0;n<11;++n){
    const auto begin=Clock::now();CHECK(c::capture(*f.project,f.lease(),*f.products,f.checkpoints));const auto written=Clock::now();
    auto loaded=c::load(f.checkpoints,saved.leaf,*f.project,*f.products);CHECK(loaded);const auto read=Clock::now();retained=loaded.value().retained_bytes();
    auto owner=mesh_import::Reimport::create(next_owner++,aid(1),f.products,f.memory);CHECK(owner);auto copied=owner.value()->restore_initial(loaded.value().product());CHECK(copied);const auto end=Clock::now();
    auto lease=owner.value()->acquire(copied.value());CHECK(lease&&lease.value()->view().revision()==saved.definition.cooked_revision);
    if(n>=2){capture[n-2]=elapsed(written-begin);load[n-2]=elapsed(read-written);restore[n-2]=elapsed(end-read);}
  }
  auto median=[](auto samples){std::sort(samples.begin(),samples.end());return samples[4];};
  std::printf("checkpoint product_bytes=%zu loaded_capacity_bytes=%zu capture_median_ms=%.4f load_median_ms=%.4f create_restore_median_ms=%.4f capture_samples_ms=",f.cooked.size(),retained,median(capture),median(load),median(restore));
  for(auto sample:capture)std::printf("%.4f,",sample);std::printf(" load_samples_ms=");for(auto sample:load)std::printf("%.4f,",sample);std::printf(" create_restore_samples_ms=");for(auto sample:restore)std::printf("%.4f,",sample);std::puts("");
}
}
int main(){roundtrip();metadata();closure();profiles();preservation();capture_lifetime();allocations();measured();std::puts("immutable selected mesh checkpoint passed");}
