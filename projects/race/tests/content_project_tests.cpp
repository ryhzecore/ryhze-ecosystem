#include "content_project.hpp"
#include "mesh_project.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <algorithm>
#include <array>

namespace p=race::content_project;
namespace {
auto bytes(std::string_view value){return std::as_bytes(std::span(value));}
struct CatalogFixture {
  FakeFiles files;
  std::vector<p::Reference> references;
  void add(AssetId id,p::Kind kind,std::string leaf,std::string encoded,bool maximum=false){
    if(maximum)encoded.resize(4096,' ');
    files.data[leaf]={bytes(encoded).begin(),bytes(encoded).end()};
    references.push_back({id,kind,std::move(leaf),digest(bytes(encoded))});
  }
  void mesh(AssetId id,mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry,
      std::string source="mesh.obj",std::string leaf="",bool maximum=false){
    auto recipe=mesh_recipe::encode({id,std::move(source),profile});CHECK(recipe);
    if(leaf.empty())leaf="mesh-"+std::to_string(id.low)+".json";
    add(id,p::Kind::Mesh,std::move(leaf),std::move(recipe).value(),maximum);
  }
  void surface(AssetId id,surface_recipe::Profile profile,std::string source="",std::string leaf="",bool maximum=false){
    const auto info=surface_pipeline::profile_info(profile);CHECK(info);
    if(source.empty())source="source"+std::string(info.value().source_extension);
    surface_recipe::Definition definition{id,std::move(source),profile,{}};
    if(profile==surface_recipe::Profile::MaterialMips)
      definition.texture=assets::surface::Reference{aid(999),digest(bytes("external cooked texture"))};
    auto recipe=surface_recipe::encode(definition);CHECK(recipe);
    if(leaf.empty())leaf="surface-"+std::to_string(id.low)+".json";
    add(id,info.value().material?p::Kind::Material:p::Kind::Texture,std::move(leaf),std::move(recipe).value(),maximum);
  }
  explicit CatalogFixture(std::size_t count=3,bool maximum=false){
    for(std::size_t n=0;n<count;++n){
      if(n%3==0)mesh(aid(n+1),mesh_pipeline::Profile::GeometryUv,"mesh.obj","",maximum);
      else surface(aid(n+1),n%3==1?surface_recipe::Profile::BmpSrgbMips:surface_recipe::Profile::MaterialMips,"","",maximum);
    }
  }
  std::vector<std::byte> encoded(){auto value=p::encode_binary(references);CHECK(value);return std::move(value).value();}
  void index(std::span<const std::byte> value){files.data["project.raceproject"]={value.begin(),value.end()};}
};
std::size_t retained(const p::Snapshot& snapshot){
  const auto storage=snapshot.storage();
  std::size_t total=sizeof(p::Snapshot)+storage.index_capacity+storage.entry_capacity*sizeof(p::Resolved);
  for(const auto& item:snapshot.entries()){
    total+=item.reference.recipe.capacity()+1;
    total+=std::visit([](const auto& recipe){return recipe.definition.source.capacity()+1+recipe.bytes.capacity();},item.recipe);
  }
  CHECK(total==snapshot.retained_bytes()&&total==storage.retained_bytes);return total;
}
void profiles_and_order(){
  CatalogFixture f(0);f.mesh({2,0});f.mesh({1,UINT64_MAX},mesh_pipeline::Profile::GeometryUv);
  const std::array profiles{surface_recipe::Profile::PpmSrgb,surface_recipe::Profile::PpmLinear,surface_recipe::Profile::PpmSrgbMips,
      surface_recipe::Profile::BmpSrgb,surface_recipe::Profile::BmpLinear,surface_recipe::Profile::BmpSrgbMips,
      surface_recipe::Profile::Material,surface_recipe::Profile::MaterialMips,surface_recipe::Profile::PpmScalarMips,surface_recipe::Profile::BmpScalarMips};
  for(std::size_t n=0;n<profiles.size();++n)f.surface({3,n+1},profiles[n]);
  const auto raw=f.encoded();std::reverse(f.references.begin(),f.references.end());CHECK(f.encoded()==raw);
  auto result=p::Snapshot::open(raw,f.files);CHECK(result&&result.value().entries().size()==profiles.size()+2);
  CHECK(result.value().revision()==digest(raw));
  CHECK(result.value().entries()[0].reference.id==AssetId{1,UINT64_MAX}&&result.value().entries()[1].reference.id==AssetId{2,0});
  std::vector<p::Reference> ordered;for(const auto& item:result.value().entries())ordered.push_back(item.reference);
  CHECK(p::encode_binary(ordered).value()==raw);
  for(std::size_t n=0;n<profiles.size();++n){
    const auto* found=result.value().find({3,n+1}).value();
    CHECK(std::get<surface_recipe::Snapshot>(found->recipe).definition.profile==profiles[n]);
    const auto info=surface_pipeline::profile_info(profiles[n]);CHECK(info);
    CHECK(found->reference.kind==(info.value().material?p::Kind::Material:p::Kind::Texture));
  }
}
void shared_mesh_wire_and_json_rejection(){
  CatalogFixture f(0);f.mesh(aid(1));f.mesh(aid(2),mesh_pipeline::Profile::GeometryUv);
  // JSON recipes retain exact whitespace bytes and pins inside binary indexes.
  for(auto& ref:f.references){f.files.data[ref.recipe].push_back(std::byte{' '});ref.revision=digest(f.files.data[ref.recipe]);}
  std::vector<mesh_project::Reference> refs;for(const auto& item:f.references)refs.push_back({item.id,item.recipe,item.revision});
  auto raw=mesh_project::encode(refs);CHECK(raw&&raw.value()==f.encoded());
  auto mesh=mesh_project::Snapshot::open(raw.value(),f.files);CHECK(mesh);f.files.reset_reads();
  auto typed=p::Snapshot::open(raw.value(),f.files);CHECK(typed&&typed.value().schema()==2&&f.files.read_count()==2);
  CHECK(typed.value().revision()==mesh.value().revision()&&typed.value().revision()==digest(raw.value()));
  for(const auto& item:mesh.value().entries()){
    const auto* found=typed.value().find(item.reference.id).value();
    CHECK(found->reference.kind==p::Kind::Mesh&&std::get<mesh_recipe::Snapshot>(found->recipe).bytes==item.recipe.bytes);
  }
  const auto retained_bytes=retained(typed.value());CHECK(p::Snapshot::open(raw.value(),f.files,retained_bytes));
  CHECK_ERROR(p::Snapshot::open(raw.value(),f.files,retained_bytes-1),ErrorCode::Exhausted);
  for(const auto schema:{1,2}){
    std::string json=" {\"schema\":"+std::to_string(schema)+",\"recipes\":[{\"asset\":\""+id_text(aid(1))+"\",";
    if(schema==2)json+="\"kind\":\"mesh\",";
    json+="\"recipe\":\""+f.references[0].recipe+"\",\"revision\":\""+hex(f.references[0].revision)+"\"}]} \n";
    f.files.reset_reads();CHECK_ERROR(p::Snapshot::open(bytes(json),f.files),ErrorCode::IncompatibleVersion);CHECK(f.files.read_count()==0);
    f.files.data["historical.json"]={bytes(json).begin(),bytes(json).end()};
    CHECK_ERROR(p::load(f.files,"historical.json"),ErrorCode::InvalidInput);CHECK(f.files.read_count()==0);
    f.files.data["renamed.raceproject"]={bytes(json).begin(),bytes(json).end()};
    CHECK_ERROR(p::load(f.files,"renamed.raceproject"),ErrorCode::IncompatibleVersion);CHECK(f.files.read_count()==1);
  }
  f.files.data.clear();CHECK(std::get<mesh_recipe::Snapshot>(typed.value().find(aid(2)).value()->recipe).definition.profile==mesh_pipeline::Profile::GeometryUv);
}
void wire32(std::vector<std::byte>& value,std::size_t at,std::uint32_t field){
  for(unsigned n=0;n<4;++n)value[at+n]=std::byte((field>>(8*n))&255U);
}
void resign(std::vector<std::byte>& value){
  CHECK(value.size()>=32);const auto pin=digest(std::span<const std::byte>(value).first(value.size()-32));
  std::copy(pin.begin(),pin.end(),value.end()-32);
}
void binary_goldens(){
  // Independently generated with Python struct.pack('<8s6I') and hashlib.sha256.
  // Whole-file goldens cover layout, endian order, zero padding and trailer hash.
  const auto empty=p::encode_binary({});CHECK(empty&&empty.value().size()==64);
  CHECK(hex(digest(empty.value()))=="8396bcc3ac7cfefe3555853aaf6dbcdd1f6bae9b6887951820c491991b75a184");
  const std::array<std::byte,32> header{
    std::byte{'R'},std::byte{'A'},std::byte{'C'},std::byte{'E'},std::byte{'P'},std::byte{'R'},std::byte{'J'},std::byte{'1'},
    std::byte{1},std::byte{},std::byte{},std::byte{},std::byte{32},std::byte{},std::byte{},std::byte{},
    std::byte{64},std::byte{},std::byte{},std::byte{},std::byte{},std::byte{},std::byte{},std::byte{},
    std::byte{152},std::byte{},std::byte{},std::byte{},std::byte{},std::byte{},std::byte{},std::byte{}};
  CHECK(std::equal(header.begin(),header.end(),empty.value().begin()));
  FakeFiles files;auto admitted=p::decode(empty.value(),files);
  CHECK(admitted&&admitted.value().format()==p::Format::Binary&&admitted.value().schema()==2&&files.read_count()==0);
  CHECK(admitted.value().entries().empty()&&admitted.value().revision()==digest(empty.value()));retained(admitted.value());
  const p::Reference ref{{7,9},p::Kind::Texture,"texture.json",digest(bytes("pinned recipe"))};
  auto one=p::encode_binary(std::span(&ref,1));CHECK(one&&one.value().size()==216);
  CHECK(hex(digest(one.value()))=="1b6bee1f37685c6cd1b2bcfb6fb7441a0ddc113a1ae198d235bfaccfe8b069d5");
  CHECK(p::binary_version==1&&p::binary_header_bytes==32&&p::binary_entry_bytes==152&&p::binary_checksum_bytes==32);
}
void binary_closure_and_ownership(){
  CatalogFixture f;f.surface(aid(4),surface_recipe::Profile::PpmScalarMips);f.surface(aid(5),surface_recipe::Profile::BmpScalarMips);
  auto encoded=p::encode_binary(f.references);CHECK(encoded);const auto raw=encoded.value();
  std::reverse(f.references.begin(),f.references.end());CHECK(p::encode_binary(f.references).value()==raw);
  f.files.data["project.raceproject"]=raw;auto admitted=p::load(f.files,"project.raceproject");CHECK(admitted);
  CHECK(admitted.value().format()==p::Format::Binary&&admitted.value().schema()==2&&admitted.value().entries().size()==5);
  CHECK(f.files.read_count()==6&&f.files.writes==0&&admitted.value().revision()==digest(raw));
  CHECK(std::equal(raw.begin(),raw.end(),admitted.value().bytes().begin(),admitted.value().bytes().end()));
  for(const auto& item:admitted.value().entries()){
    CHECK(std::visit([&](const auto& recipe){return recipe.bytes==f.files.data[item.reference.recipe]&&recipe.revision==item.reference.revision;},item.recipe));
  }
  CHECK(std::get<surface_recipe::Snapshot>(admitted.value().find(aid(4)).value()->recipe).definition.profile==surface_recipe::Profile::PpmScalarMips);
  CHECK(std::get<surface_recipe::Snapshot>(admitted.value().find(aid(5)).value()->recipe).definition.profile==surface_recipe::Profile::BmpScalarMips);
  CHECK_ERROR(admitted.value().find(aid(99)),ErrorCode::NotFound);
  const auto storage=retained(admitted.value());const auto recipe=std::get<surface_recipe::Snapshot>(admitted.value().find(aid(3)).value()->recipe).bytes;
  encoded.value().clear();f.files.data.clear();f.references.clear();auto moved=std::move(admitted).value();
  CHECK(moved.format()==p::Format::Binary&&retained(moved)==storage&&moved.revision()==digest(raw));
  CHECK(admitted.value().format()==p::Format::Binary&&admitted.value().schema()==0&&admitted.value().revision()==Digest{});
  CHECK(admitted.value().entries().empty()&&admitted.value().bytes().empty()&&retained(admitted.value())==sizeof(p::Snapshot));
  FakeFiles files;auto target=p::decode(p::encode_binary({}).value(),files);CHECK(target&&target.value().format()==p::Format::Binary);
  target.value()=std::move(moved);CHECK(target.value().format()==p::Format::Binary&&retained(target.value())==storage);
  CHECK(std::get<surface_recipe::Snapshot>(target.value().find(aid(3)).value()->recipe).bytes==recipe);
  CHECK(moved.format()==p::Format::Binary&&moved.schema()==0&&moved.revision()==Digest{}&&retained(moved)==sizeof(p::Snapshot));
}
void binary_malformed_before_io(){
  CatalogFixture f;const auto raw=p::encode_binary(f.references).value();
  const auto rejected=[&](const auto& bad){f.files.reset_reads();CHECK(!p::Snapshot::open(bad,f.files));CHECK(f.files.read_count()==0);};
  for(std::size_t n=0;n<raw.size();++n)rejected(std::span<const std::byte>(raw).first(n));
  for(std::size_t n=0;n<raw.size();++n){auto bad=raw;bad[n]^=std::byte{1};rejected(bad);}
  auto bad=raw;bad.push_back(std::byte{});rejected(bad);
  // Re-sign malformed metadata so the field validators, not the checksum, reject it.
  for(const auto& [offset,value]:std::array<std::pair<std::size_t,std::uint32_t>,14>{{
      {8,0},{8,2},{12,31},{12,33},{16,0},{16,UINT32_MAX},{20,0},{20,2},{20,33},{20,UINT32_MAX},
      {24,151},{24,153},{28,1},{28,UINT32_MAX}}}){
    bad=raw;wire32(bad,offset,value);resign(bad);rejected(bad);
  }
  constexpr std::size_t last=32+2*152;
  for(const auto kind:{0U,4U,257U,UINT32_MAX}){bad=raw;wire32(bad,last+48,kind);resign(bad);rejected(bad);}
  for(const auto length:{0U,97U,UINT32_MAX}){bad=raw;wire32(bad,last+52,length);resign(bad);rejected(bad);}
  bad=raw;std::fill_n(bad.begin()+last,16,std::byte{});resign(bad);rejected(bad);
  bad=raw;std::fill_n(bad.begin()+last+16,32,std::byte{});resign(bad);rejected(bad);
  bad=raw;bad[last+56+95]=std::byte{1};resign(bad);rejected(bad);
  for(const auto name:{"../recipe.json","UPPER.JSON","recipe.obj","a/b.json","con.json",".recipe.json"}){
#ifndef _WIN32
    if(std::string_view{name}=="con.json")continue;
#endif
    bad=raw;const auto leaf=bytes(name);wire32(bad,last+52,static_cast<std::uint32_t>(leaf.size()));
    std::fill_n(bad.begin()+last+56,96,std::byte{});std::copy(leaf.begin(),leaf.end(),bad.begin()+last+56);resign(bad);rejected(bad);
  }
  for(const auto ch:{std::byte{},std::byte{0x80}}){bad=raw;bad[last+56]=ch;resign(bad);rejected(bad);}
  bad=raw;std::copy_n(bad.begin()+32,16,bad.begin()+last);resign(bad);rejected(bad); // Duplicate ID in the last record.
  bad=raw;std::copy_n(bad.begin()+32+52,100,bad.begin()+last+52);resign(bad);rejected(bad); // Duplicate recipe leaf.
  bad=raw;std::swap_ranges(bad.begin()+32,bad.begin()+32+152,bad.begin()+32+152);resign(bad);rejected(bad);
  for(const auto name:{"../recipe.json","UPPER.JSON","recipe.obj","a/b.json","con.json","",".recipe.json"}){
#ifndef _WIN32
    if(std::string_view{name}=="con.json")continue;
#endif
    auto refs=f.references;refs[0].recipe=name;CHECK(!p::encode_binary(refs));
  }
  for(const auto kind:{0,4,255}){auto refs=f.references;refs[0].kind=static_cast<p::Kind>(kind);CHECK(!p::encode_binary(refs));}
  auto refs=f.references;refs[0].id={};CHECK(!p::encode_binary(refs));refs=f.references;refs[0].revision={};CHECK(!p::encode_binary(refs));
  refs=f.references;refs[1].id=refs[0].id;CHECK_ERROR(p::encode_binary(refs),ErrorCode::Duplicate);
  refs=f.references;refs[1].recipe=refs[0].recipe;CHECK_ERROR(p::encode_binary(refs),ErrorCode::Duplicate);
  CHECK(f.files.read_count()==0&&f.files.writes==0);
}
void binary_limits_and_extensions(){
  CatalogFixture f(32,true);const auto raw=p::encode_binary(f.references).value();CHECK(raw.size()==4928);
  auto full=p::Snapshot::open(raw,f.files);CHECK(full&&full.value().entries().size()==32&&f.files.read_count()==32);
  const auto storage=retained(full.value());CHECK(storage>32*4096&&storage<p::maximum_retained_bytes);
  CHECK(p::Snapshot::open(raw,f.files,storage));CHECK_ERROR(p::Snapshot::open(raw,f.files,storage-1),ErrorCode::Exhausted);
  f.files.reset_reads();CHECK_ERROR(p::Snapshot::open(raw,f.files,sizeof(p::Snapshot)+raw.size()-1),ErrorCode::Exhausted);CHECK(f.files.read_count()==0);
  CHECK_ERROR(p::Snapshot::open(raw,f.files,0),ErrorCode::InvalidInput);
  CHECK_ERROR(p::Snapshot::open(raw,f.files,p::maximum_retained_bytes+1),ErrorCode::InvalidInput);
  f.references.push_back({aid(33),p::Kind::Mesh,"extra.json",f.references[0].revision});CHECK_ERROR(p::encode_binary(f.references),ErrorCode::Exhausted);
  CatalogFixture long_name(0);long_name.mesh(aid(1),mesh_pipeline::Profile::Geometry,std::string(92,'b')+".obj",std::string(91,'a')+".json");
  auto longest=p::encode_binary(long_name.references);CHECK(longest);auto named=p::decode(longest.value(),long_name.files);CHECK(named);
  CHECK(named.value().entries()[0].reference.recipe.size()==96&&std::get<mesh_recipe::Snapshot>(named.value().entries()[0].recipe).definition.source.size()==96);
  long_name.surface(aid(2),surface_recipe::Profile::BmpSrgb,std::string(92,'c')+".bmp");
  auto long_surface=p::decode(long_name.encoded(),long_name.files);CHECK(long_surface);retained(long_surface.value());
  CHECK(std::get<surface_recipe::Snapshot>(long_surface.value().find(aid(2)).value()->recipe).definition.source.size()==96);
  long_name.references[0].recipe.insert(0,1,'a');CHECK(!p::encode_binary(long_name.references));
  CatalogFixture formats;const auto binary=formats.encoded();
  CHECK(p::valid_index_leaf("project.raceproject")&&!p::valid_index_leaf("project.json"));
  CHECK(p::valid_new_index_leaf("project.raceproject")&&!p::valid_new_index_leaf("project.json"));
  for(const auto name:{"../project.raceproject","PROJECT.RACEPROJECT","a/b.json","con.raceproject",".raceproject","project.obj",""}){
#ifndef _WIN32
    if(std::string_view{name}=="con.raceproject")continue;
#endif
    CHECK(!p::valid_index_leaf(name)&&!p::valid_new_index_leaf(name));CHECK(!p::load(formats.files,name));
  }
  CHECK(formats.files.read_count()==0);
  formats.files.data["wrong.json"]=binary;
  CHECK_ERROR(p::load(formats.files,"wrong.json"),ErrorCode::InvalidInput);CHECK(formats.files.read_count()==0);
  const std::string legacy="{\"schema\":2,\"recipes\":[]}\n";
  formats.files.data["wrong.raceproject"]={bytes(legacy).begin(),bytes(legacy).end()};
  CHECK_ERROR(p::load(formats.files,"wrong.raceproject"),ErrorCode::IncompatibleVersion);CHECK(formats.files.read_count()==1);
}
void binary_closure_failures_and_faults(){
  CatalogFixture f;const auto raw=p::encode_binary(f.references).value();f.files.data["project.raceproject"]=raw;
  auto prior=p::load(f.files,"project.raceproject");CHECK(prior);const auto storage=retained(prior.value());
  const auto recipe=std::get<surface_recipe::Snapshot>(prior.value().find(aid(3)).value()->recipe).bytes;
  f.files.data[f.references[2].recipe].push_back(std::byte{' '});
  CHECK_ERROR(p::load(f.files,"project.raceproject"),ErrorCode::InvalidDependency);
  CHECK(prior.value().revision()==digest(raw)&&retained(prior.value())==storage&&std::get<surface_recipe::Snapshot>(prior.value().find(aid(3)).value()->recipe).bytes==recipe);
  f.files.data.erase(f.references[2].recipe);CHECK_ERROR(p::load(f.files,"project.raceproject"),ErrorCode::NotFound);
  f.files.data[f.references[2].recipe]=recipe;
  auto refs=f.references;refs[2].id=aid(99);CHECK_ERROR(p::decode(p::encode_binary(refs).value(),f.files),ErrorCode::InvalidDependency);
  refs=f.references;refs[2].revision[0]^=std::byte{1};CHECK_ERROR(p::decode(p::encode_binary(refs).value(),f.files),ErrorCode::InvalidDependency);
  for(const auto n:{1U,2U}){refs=f.references;refs[n].kind=n==1?p::Kind::Material:p::Kind::Texture;CHECK_ERROR(p::decode(p::encode_binary(refs).value(),f.files),ErrorCode::InvalidDependency);}
  refs=f.references;refs[0].kind=p::Kind::Texture;CHECK(!p::decode(p::encode_binary(refs).value(),f.files));
  refs=f.references;refs[1].kind=p::Kind::Mesh;CHECK(!p::decode(p::encode_binary(refs).value(),f.files));
  struct Reserved final:FakeFiles {
    std::string excess;
    Result<std::vector<std::byte>> read(std::string_view name,std::size_t limit) override{
      auto value=FakeFiles::read(name,limit);if(value&&name==excess)value.value().reserve(limit+1);return value;
    }
  } reserved;
  reserved.data=f.files.data;reserved.excess="project.raceproject";CHECK_ERROR(p::load(reserved,"project.raceproject"),ErrorCode::Exhausted);
  reserved.excess=f.references[1].recipe;CHECK_ERROR(p::load(reserved,"project.raceproject"),ErrorCode::Exhausted);
  std::size_t failures{};
  for(std::size_t n=0;n<192;++n){
    desktop::profile::testing::fail_allocation_after(n);auto loaded=p::load(f.files,"project.raceproject");desktop::profile::testing::fail_next_allocation_attempts(0);
    if(loaded)break;CHECK(loaded.error().code==ErrorCode::Exhausted);++failures;
  }
  CHECK(failures>4&&failures<192&&f.files.writes==0&&f.files.data["project.raceproject"]==raw);
  desktop::profile::testing::fail_allocation_after(0);auto failed=p::encode_binary(f.references);desktop::profile::testing::fail_next_allocation_attempts(0);
  CHECK_ERROR(failed,ErrorCode::Exhausted);
  desktop::profile::testing::fail_allocation_after(1);auto once=p::encode_binary(f.references);desktop::profile::testing::fail_next_allocation_attempts(0);
  CHECK(once&&once.value()==raw); // Encoding needs exactly one bounded allocation.
  f.files.throw_read=true;CHECK_ERROR(p::load(f.files,"project.raceproject"),ErrorCode::Exhausted);
}
} // namespace
int main(){profiles_and_order();shared_mesh_wire_and_json_rejection();
  binary_goldens();binary_closure_and_ownership();binary_malformed_before_io();binary_limits_and_extensions();binary_closure_failures_and_faults();
  std::puts("binary-only typed catalog closure, wire goldens, JSON rejection, capacity and ownership passed");}
