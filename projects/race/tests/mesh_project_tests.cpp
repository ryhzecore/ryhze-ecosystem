#include "mesh_project.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <algorithm>
#include <array>

namespace p=race::mesh_project;
namespace {
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
auto bytes(std::span<const std::byte> value){return value;}
struct Fixture {
  FakeFiles files;
  std::vector<p::Reference> references;
  explicit Fixture(std::size_t count=2,bool maximum=false){
    for(std::size_t n=0;n<count;++n){
      const auto name="recipe-"+std::to_string(n)+".json";
      auto recipe=mesh_recipe::encode({aid(n+1),"source.obj"});CHECK(recipe);
      if(maximum)recipe.value().resize(mesh_recipe::maximum_bytes,' ');
      auto raw=bytes(recipe.value());files.data[name]={raw.begin(),raw.end()};references.push_back({aid(n+1),name,digest(raw)});
    }
  }
  std::vector<std::byte> encoded(){auto text=p::encode(references);CHECK(text);return std::move(text).value();}
  void index(std::span<const std::byte> value){files.data["project.raceproject"]={value.begin(),value.end()};}
};
void roundtrip(){
  Fixture f;auto canonical=f.encoded();f.index(canonical);auto result=p::load(f.files,"project.raceproject");CHECK(result);
  CHECK(result.value().entries().size()==2&&result.value().revision()==digest(bytes(canonical)));
  const auto* selected=result.value().find(aid(1)).value();CHECK(selected->recipe.definition.source=="source.obj");
  CHECK_ERROR(result.value().find(aid(0)),ErrorCode::NotFound);
  const auto owned=selected->recipe.bytes;f.files.data.clear();std::fill(canonical.begin(),canonical.end(),std::byte{'x'});
  CHECK(selected->recipe.bytes==owned&&selected->recipe.definition.id==aid(1));
  auto moved=std::move(result).value();CHECK(moved.find(aid(1)).value()->recipe.bytes==owned);
  Fixture empty(0);auto text=empty.encoded();CHECK(text.size()==64&&hex(digest(text))=="8396bcc3ac7cfefe3555853aaf6dbcdd1f6bae9b6887951820c491991b75a184");auto e=p::Snapshot::open(bytes(text),empty.files);CHECK(e&&e.value().entries().empty()&&empty.files.read_count()==0);
  CHECK_ERROR(e.value().find(aid(1)),ErrorCode::NotFound);
  Fixture order;auto original=order.encoded();std::reverse(order.references.begin(),order.references.end());CHECK(order.encoded()==original);
  auto loaded=p::Snapshot::open(original,order.files);CHECK(loaded&&loaded.value().revision()==digest(original));
  CHECK(loaded.value().entries()[0].reference.id==aid(1)&&loaded.value().entries()[1].reference.id==aid(2));
  std::vector<p::Reference> resolved;for(const auto& item:loaded.value().entries())resolved.push_back(item.reference);CHECK(p::encode(resolved).value()==original);
  Fixture words(0);const std::array<AssetId,3> identities{{{2,1},{1,UINT64_MAX},{2,0}}};
  for(std::size_t n=0;n<identities.size();++n){
    auto raw=mesh_recipe::encode({identities[n],"source.obj"});CHECK(raw);const auto name="words-"+std::to_string(n)+".json";
    words.files.data[name]={bytes(raw.value()).begin(),bytes(raw.value()).end()};words.references.push_back({identities[n],name,digest(bytes(raw.value()))});
  }
  auto indexed=p::Snapshot::open(bytes(words.encoded()),words.files);CHECK(indexed);
  CHECK(indexed.value().entries()[0].reference.id==identities[1]&&indexed.value().entries()[1].reference.id==identities[2]);
  for(const auto id:identities)CHECK(indexed.value().find(id).value()->recipe.definition.id==id);
  CHECK(!indexed.value().find({2,2})&&!indexed.value().find({0,UINT64_MAX}));
}
void bounds(){
  Fixture f(p::maximum_entries,true);auto text=f.encoded();CHECK(text.size()==4928);
  auto full=p::Snapshot::open(bytes(text),f.files);CHECK(full&&full.value().entries().size()==32&&f.files.read_count()==32);
  const auto retained=full.value().retained_bytes();CHECK(retained>32*4096&&retained<p::maximum_retained_bytes);
  CHECK(p::Snapshot::open(bytes(text),f.files,retained));CHECK_ERROR(p::Snapshot::open(bytes(text),f.files,retained-1),ErrorCode::Exhausted);
  CHECK_ERROR(p::Snapshot::open(bytes(text),f.files,sizeof(p::Snapshot)-1),ErrorCode::Exhausted);
  CHECK_ERROR(p::Snapshot::open(bytes(text),f.files,0),ErrorCode::InvalidInput);
  CHECK_ERROR(p::Snapshot::open(bytes(text),f.files,p::maximum_retained_bytes+1),ErrorCode::InvalidInput);
  text.resize(p::maximum_bytes+1,std::byte{});CHECK_ERROR(p::Snapshot::open(bytes(text),f.files),ErrorCode::Exhausted);
  f.references.push_back({aid(33),"extra.json",f.references[0].revision});CHECK_ERROR(p::encode(f.references),ErrorCode::Exhausted);
}
void wire32(std::vector<std::byte>& value,std::size_t at,std::uint32_t field){
  for(unsigned n=0;n<4;++n)value[at+n]=std::byte((field>>(8*n))&255U);
}
void resign(std::vector<std::byte>& value){
  const auto pin=digest(std::span<const std::byte>(value).first(value.size()-32));
  std::copy(pin.begin(),pin.end(),value.end()-32);
}
void too_many(){
  Fixture f(32);auto raw=f.encoded();wire32(raw,20,33);resign(raw);
  CHECK_ERROR(p::Snapshot::open(raw,f.files),ErrorCode::Exhausted);CHECK(f.files.read_count()==0);
}
void storage_and_moves(){
  Fixture f;const auto long_leaf=std::string(91,'a')+".json",long_source=std::string(92,'b')+".obj";
  auto recipe=mesh_recipe::encode({aid(1),long_source});CHECK(recipe);
  f.references[0].recipe=long_leaf;f.references[0].revision=digest(bytes(recipe.value()));f.files.data[long_leaf]={bytes(recipe.value()).begin(),bytes(recipe.value()).end()};
  auto encoded=f.encoded();auto admitted=p::Snapshot::open(bytes(encoded),f.files);CHECK(admitted);
  auto sum=[](const p::Snapshot& snapshot){
    const auto storage=snapshot.storage();std::size_t total=sizeof(p::Snapshot)+storage.index_capacity+storage.entry_capacity*sizeof(p::Resolved);
    for(const auto& item:snapshot.entries())total+=item.reference.recipe.capacity()+1+item.recipe.definition.source.capacity()+1+item.recipe.bytes.capacity();
    CHECK(total==snapshot.retained_bytes()&&total==storage.retained_bytes);return total;
  };
  const auto retained=sum(admitted.value());
  // Encoding sorts borrowed views without moving the owned recipe strings.
  std::reverse(f.references.begin(),f.references.end());CHECK(f.encoded()==encoded);
  auto moved=std::move(admitted).value();CHECK(sum(moved)==retained);
  CHECK(admitted.value().entries().empty()&&admitted.value().bytes().empty()&&admitted.value().revision()==Digest{}&&sum(admitted.value())==sizeof(p::Snapshot));
  Fixture empty(0);auto target=p::Snapshot::open(bytes(empty.encoded()),empty.files);CHECK(target);target.value()=std::move(moved);
  CHECK(sum(target.value())==retained&&sum(moved)==sizeof(p::Snapshot)&&moved.revision()==Digest{});
  auto other=p::Snapshot::open(bytes(f.encoded()),f.files);CHECK(other);other.value()=std::move(target).value();CHECK(sum(other.value())==retained&&sum(target.value())==sizeof(p::Snapshot));
  auto bad=f.references;bad[0].recipe="a"+long_leaf;CHECK(!p::encode(bad));
}
void malformed(){
  Fixture f;const auto original=f.encoded();
  const auto rejected=[&](std::span<const std::byte> raw){f.files.reset_reads();CHECK(!p::Snapshot::open(raw,f.files));CHECK(f.files.read_count()==0);};
  // Both old schemas, even renamed as .raceproject, fail before recipe I/O.
  for(const auto schema:{1,2}){
    std::string json=" {\"schema\":"+std::to_string(schema)+",\"recipes\":[{\"asset\":\""+id_text(aid(1))+"\",";
    if(schema==2)json+="\"kind\":\"mesh\",";
    json+="\"recipe\":\""+f.references[0].recipe+"\",\"revision\":\""+hex(f.references[0].revision)+"\"}]} \n";
    rejected(bytes(json));f.files.data["old.json"]={bytes(json).begin(),bytes(json).end()};
    CHECK_ERROR(p::load(f.files,"old.json"),ErrorCode::InvalidInput);CHECK(f.files.read_count()==0);
    f.files.data["renamed.raceproject"]={bytes(json).begin(),bytes(json).end()};
    CHECK_ERROR(p::load(f.files,"renamed.raceproject"),ErrorCode::IncompatibleVersion);CHECK(f.files.read_count()==1);
  }
  f.files.data["binary.json"]=original;f.files.reset_reads();
  CHECK_ERROR(p::load(f.files,"binary.json"),ErrorCode::InvalidInput);CHECK(f.files.read_count()==0);
  for(std::size_t n=0;n<original.size();++n)rejected(std::span<const std::byte>(original).first(n));
  for(std::size_t n=0;n<original.size();++n){auto bad=original;bad[n]^=std::byte{1};rejected(bad);}
  auto bad=original;bad.push_back(std::byte{});rejected(bad);
  for(const auto& [at,value]:std::array<std::pair<std::size_t,std::uint32_t>,8>{{
      {8,2},{12,0},{16,UINT32_MAX},{20,33},{24,0},{28,1},{32+52,0},{32+52,97}}}){
    bad=original;wire32(bad,at,value);resign(bad);rejected(bad);
  }
  for(const auto kind:{0U,4U,257U,UINT32_MAX}){bad=original;wire32(bad,32+152+48,kind);resign(bad);rejected(bad);}
  // A valid non-mesh record anywhere rejects the complete catalog before reading
  // even the earlier valid mesh recipe; no partial mesh subset is admitted.
  for(const auto kind:{2U,3U}){
    bad=original;wire32(bad,32+152+48,kind);resign(bad);f.files.reset_reads();
    CHECK_ERROR(p::Snapshot::open(bad,f.files),ErrorCode::InvalidDependency);CHECK(f.files.read_count()==0);
  }
  bad=original;bad[32+56+95]=std::byte{1};resign(bad);rejected(bad);
  bad=original;std::copy_n(bad.begin()+32,16,bad.begin()+32+152);resign(bad);rejected(bad);
  bad=original;std::copy_n(bad.begin()+32+52,100,bad.begin()+32+152+52);resign(bad);rejected(bad);
  bad=original;std::swap_ranges(bad.begin()+32,bad.begin()+32+152,bad.begin()+32+152);resign(bad);rejected(bad);
  for(const auto leaf:{"../recipe.json","RECIPE.JSON","recipe.obj","a/b.json","con.json","",".recipe.json"}){
#ifndef _WIN32
    if(std::string_view{leaf}=="con.json")continue;
#endif
    auto refs=f.references;refs[0].recipe=leaf;CHECK(!p::encode(refs));
  }
  auto refs=f.references;refs[0].id={};CHECK(!p::encode(refs));refs=f.references;refs[0].revision={};CHECK(!p::encode(refs));
  refs=f.references;refs[1].id=refs[0].id;CHECK_ERROR(p::encode(refs),ErrorCode::Duplicate);
  refs=f.references;refs[1].recipe=refs[0].recipe;CHECK_ERROR(p::encode(refs),ErrorCode::Duplicate);
  CHECK(f.files.read_count()==0);
}
void closure(){
  Fixture f;const auto text=f.encoded();auto admitted=p::Snapshot::open(bytes(text),f.files);CHECK(admitted);
  const auto old=admitted.value().find(aid(1)).value()->recipe.bytes;
  f.files.data[f.references[1].recipe].push_back(std::byte{' '});CHECK_ERROR(p::Snapshot::open(bytes(text),f.files),ErrorCode::InvalidDependency);
  CHECK(admitted.value().find(aid(1)).value()->recipe.bytes==old);
  f.files.data.erase(f.references[1].recipe);CHECK_ERROR(p::Snapshot::open(bytes(text),f.files),ErrorCode::NotFound);
  f.files.data[f.references[1].recipe]={std::byte{'x'}};CHECK(!p::Snapshot::open(bytes(text),f.files));
  Fixture other;auto refs=other.references;refs[0].id=aid(99);CHECK_ERROR(p::Snapshot::open(bytes(p::encode(refs).value()),other.files),ErrorCode::InvalidDependency);
  refs=other.references;refs[0].revision[0]^=std::byte{1};CHECK_ERROR(p::Snapshot::open(bytes(p::encode(refs).value()),other.files),ErrorCode::InvalidDependency);
  CHECK(other.files.writes==0&&f.files.writes==0);
}
void faults(){
  Fixture f;auto text=f.encoded();f.index(text);CHECK(!p::load(f.files,"../project.json"));CHECK(!p::load(f.files,"project.obj"));
  struct Reserved final:FakeFiles {Result<std::vector<std::byte>> read(std::string_view,std::size_t) override{std::vector<std::byte> bytes{std::byte{'x'}};bytes.reserve(p::maximum_bytes+1);return bytes;}} reserved;
  CHECK_ERROR(p::load(reserved,"project.raceproject"),ErrorCode::Exhausted);
  std::size_t failures{};
  for(std::size_t n=0;n<128;++n){
    desktop::profile::testing::fail_allocation_after(n);auto result=p::load(f.files,"project.raceproject");desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;CHECK(result.error().code==ErrorCode::Exhausted);++failures;
  }
  CHECK(failures>4&&failures<128);CHECK(f.files.data["project.raceproject"]==std::vector<std::byte>(bytes(text).begin(),bytes(text).end()));
  failures=0;for(std::size_t n=0;n<256;++n){
    desktop::profile::testing::fail_allocation_after(n);auto result=p::encode(f.references);desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;CHECK(result.error().code==ErrorCode::Exhausted);++failures;
  }
  CHECK(failures>0&&failures<256);f.files.throw_read=true;CHECK_ERROR(p::load(f.files,"project.raceproject"),ErrorCode::Exhausted);
}
void measured(){
  Fixture f(32);const auto text=f.encoded();auto admitted=p::Snapshot::open(bytes(text),f.files);CHECK(admitted);
  std::array<double,9> admission{},lookup{};std::size_t count{},allocations{};
  for(unsigned trial=0;trial<11;++trial){
    auto start=std::chrono::steady_clock::now();for(unsigned n=0;n<16;++n){auto candidate=p::Snapshot::open(bytes(text),f.files);CHECK(candidate);}
    auto end=std::chrono::steady_clock::now();const auto open_us=std::chrono::duration<double,std::micro>(end-start).count();start=end;
    for(unsigned n=0;n<32768;++n){auto item=admitted.value().find(aid(n%33+1));if(item)count+=static_cast<std::size_t>(item.value()->reference.id.low);}
    const auto find_us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();if(trial>=2){admission[trial-2]=open_us;lookup[trial-2]=find_us;}
  }
  {desktop::profile::AllocationScope scope;const auto before=desktop::profile::allocation_snapshot();for(unsigned n=0;n<32768;++n)(void)admitted.value().find(aid(n%33+1));const auto after=desktop::profile::allocation_snapshot();CHECK(!after.overflowed);allocations=static_cast<std::size_t>(after.requests-before.requests);}
  auto sorted=admission,finds=lookup;std::sort(sorted.begin(),sorted.end());std::sort(finds.begin(),finds.end());CHECK(count==11*993*528&&allocations==0);
  std::printf("project entries=32 bytes=%zu retained=%zu 16_admissions_median_us=%.4f 32768_lookups_median_us=%.4f lookup_allocations=%zu admission_samples_us=",text.size(),admitted.value().retained_bytes(),sorted[4],finds[4],allocations);
  for(auto value:admission)std::printf("%.4f,",value);std::printf(" lookup_samples_us=");for(auto value:lookup)std::printf("%.4f,",value);std::puts("");
}
}
int main(){roundtrip();bounds();too_many();storage_and_moves();malformed();closure();faults();measured();std::puts("binary mesh project closure, fail-closed JSON and mixed-kind rejection passed");}
