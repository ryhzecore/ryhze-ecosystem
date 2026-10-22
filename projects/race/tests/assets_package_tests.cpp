#include "assets_support.hpp"
#include "race/assets/package.hpp"
#include "race/assets/surface.hpp"
#include <array>
#include <optional>

namespace {
namespace p=race::assets::pak;
// Independently assembled with Python struct/hashlib, not the RACE codec.
// These opaque bytes certify only the container, never desktop content.
std::vector<std::byte> golden(){
  constexpr std::string_view hex=
    "5241434550414b310100000040000000780100000000000002000000010000007000000030000000400000000000000050010000000000000000000000000000000000000000000001000000000000004f48f7207cebf92638debb5694e4a8677350cf88a9e98e73c6c466b6b1d4389002000000010000000100000000000000000000000000000050010000000000000500000000000000050000000000000000000000010000000000000000000000000000000000000002000000000000004234f1064f32788dc9487507f9eeecd0cdf7df6bd010fd1dad8026226e22281301000000010000000100000000000000000000000000000055010000000000000300000000000000030000000000000001000000000000000000000000000000000000000000000002000000000000004234f1064f32788dc9487507f9eeecd0cdf7df6bd010fd1dad8026226e2228137363656e65007fffe93d67626f5ce3fbf344d6f95343321c42dc60a9423b350b33b0b5fa5de088d6";
  const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
  std::vector<std::byte> out;for(std::size_t i=0;i<hex.size();i+=2)out.push_back(std::byte(digit(hex[i])*16+digit(hex[i+1])));return out;
}
void put(std::vector<std::byte>& b,std::size_t at,std::uint64_t v,std::size_t width=8){
  for(std::size_t n=0;n<width;++n)b[at+n]=std::byte((v>>(8*n))&255);
}
void resign(std::vector<std::byte>& b){auto hash=assets::digest(std::span<const std::byte>(b).first(b.size()-32));std::copy(hash.begin(),hash.end(),b.end()-32);}
void wire(){
  auto b=golden();CHECK(b.size()==376);
  CHECK(assets::hex(assets::digest(b))=="675929d68a40dfab4c855d8db4d02af62924dfbbbf7864847eb0f6577a4bbbe5");
  auto index=p::validate(b);CHECK(index&&index.value().size()==2);
  CHECK(index.value()[0].offset==336&&index.value()[0].bytes==5&&index.value()[0].kind==p::Kind::Scene);
  CHECK(index.value()[1].offset==341&&index.value()[1].bytes==3&&index.value()[1].kind==p::Kind::Mesh);
  CHECK(index.value()[0].dependencies==std::vector<p::Key>{index.value()[1].key});
  std::array<p::Input,2> inputs{{{{0,2},p::Kind::Mesh,std::span(b).subspan(341,3),{}},{{0,1},p::Kind::Scene,std::span(b).subspan(336,5),{index.value()[1].key}}}};
  auto encoded=p::encode(inputs);CHECK(encoded&&encoded.value()==b);std::reverse(inputs.begin(),inputs.end());CHECK(p::encode(inputs).value()==b);
  for(std::size_t n=0;n<b.size();++n){CHECK(!p::validate(std::span(b).first(n)));CHECK(!p::Package::open(std::span(b).first(n)));}
  for(std::size_t n=0;n<b.size();++n){auto bad=b;bad[n]^=std::byte{1};CHECK(!p::validate(bad));}
  struct Mutation{std::size_t offset,width;std::uint64_t value;};
  constexpr Mutation mutations[]{
    {8,4,2},{12,4,63},{16,8,UINT64_MAX},{24,4,0},{24,4,129},{24,4,UINT32_MAX},
    {28,4,UINT32_MAX},{32,4,111},{36,4,47},{40,8,65},{48,8,UINT64_MAX},{56,8,1},
    {64+8,8,0},{64+48,4,5},{64+52,4,2},{64+56,4,2},{64+60,4,1},{64+64,8,1},
    {64+72,8,335},{64+72,8,337},{64+72,8,UINT64_MAX},{64+80,8,0},{64+80,8,UINT64_MAX},
    {64+88,8,4},{64+96,4,UINT32_MAX},{64+100,4,128},{64+100,4,0},{64+104,8,1},
    {176+72,8,340},{176+72,8,342},{176+80,8,4},{176+96,4,0},{176+100,4,1},
    {288+8,8,99},{288+8,8,0},{336,1,0}
  };
  for(const auto& m:mutations){auto bad=b;put(bad,m.offset,m.value,m.width);resign(bad);CHECK(!p::validate(bad));}
  auto bad=b;std::copy_n(b.begin()+64,48,bad.begin()+176);resign(bad);CHECK(!p::validate(bad));
  bad=b;std::copy_n(b.begin()+64,48,bad.begin()+288);resign(bad);CHECK(!p::validate(bad));
  bad=b;put(bad,176+48,2,4);resign(bad);CHECK(!p::validate(bad));
  auto exact=p::Limits{2,1,5,376};CHECK(p::validate(b,exact));CHECK(p::encode(inputs,exact).value()==b);
  auto short_limit=exact;--short_limit.package_bytes;CHECK_ERROR(p::validate(b,short_limit),ErrorCode::Exhausted);CHECK_ERROR(p::encode(inputs,short_limit),ErrorCode::Exhausted);
  short_limit=exact;--short_limit.chunk_bytes;CHECK_ERROR(p::validate(b,short_limit),ErrorCode::Exhausted);
  short_limit=exact;--short_limit.entries;CHECK_ERROR(p::validate(b,short_limit),ErrorCode::Exhausted);
  short_limit=exact;short_limit.dependencies=0;CHECK_ERROR(p::validate(b,short_limit),ErrorCode::Exhausted);
  for(auto invalid:{p::Limits{0,1,5,376},p::Limits{129,1,5,376},p::Limits{2,128,5,376},p::Limits{2,1,0,376},p::Limits{2,1,5,208},p::Limits{2,1,5,64*1024*1024+1},p::Limits{2,1,16*1024*1024+1,376}}){CHECK(!p::validate_limits(invalid));CHECK(!p::encode(inputs,invalid));CHECK(!p::Package::open(b,invalid));}
  std::vector<std::byte> one{std::byte{7}};p::Input minimal{{3,4},p::Kind::Mesh,one,{}};
  CHECK(p::encode({&minimal,1},p::Limits{1,0,1,209}).value().size()==209);
  auto duplicate=inputs;duplicate[1]=duplicate[0];CHECK(!p::encode(duplicate));
  inputs[0].id={};CHECK(!p::encode(inputs));
  std::vector<p::Input> full;std::vector<p::Key> dependencies;
  for(std::uint64_t n=1;n<=127;++n){full.push_back({{8,n},p::Kind::Mesh,one,{}});dependencies.push_back({{8,n},assets::digest(one)});}
  full.push_back({{9,1},p::Kind::Scene,one,dependencies});auto maximal=p::encode(full);CHECK(maximal&&p::validate(maximal.value()).value().size()==128);
  std::reverse(full.back().dependencies.begin(),full.back().dependencies.end());std::reverse(full.begin(),full.end());CHECK(p::encode(full).value()==maximal.value());
  full.front().dependencies[0]=full.front().dependencies[1];CHECK(!p::encode(full));
}
void typed_dependencies(){
  const std::array<std::byte,1> scene{std::byte{1}},mesh{std::byte{2}},material{std::byte{3}},texture{std::byte{4}};
  const p::Key mesh_key{{88,2},assets::digest(mesh)},material_key{{88,3},assets::digest(material)},texture_key{{88,4},assets::digest(texture)};
  std::vector<p::Input> entries{{{88,1},p::Kind::Scene,scene,{mesh_key,material_key}},
    {mesh_key.id,p::Kind::Mesh,mesh,{}},{material_key.id,p::Kind::Material,material,{texture_key}},{texture_key.id,p::Kind::Texture,texture,{}}};
  auto bytes=p::encode(entries);CHECK(bytes);auto package=p::Package::open(bytes.value());CHECK(package);
  auto files=package.value().files();CHECK(files);
  CHECK(files.value()->read(assets::hex(material_key.revision)+".rasset",1).value()==std::vector<std::byte>{std::byte{3}});
  CHECK(files.value()->read(assets::hex(texture_key.revision)+".rasset",1).value()==std::vector<std::byte>{std::byte{4}});
  CHECK_ERROR(files.value()->read(assets::hex(texture_key.revision)+".rscene",1),ErrorCode::NotFound);
  auto uv=entries;uv[1].schema=2;auto uv_bytes=p::encode(uv);CHECK(uv_bytes);
  CHECK(p::validate(uv_bytes.value()).value()[1].schema==2);
  uv[1].schema=3;CHECK(!p::encode(uv));
  // Domain schema tags are independent of the unchanged container and closure.
  for(std::uint32_t mesh_schema:{1u,2u})for(std::uint32_t material_schema:{1u,2u,3u})for(std::uint32_t texture_schema:{1u,2u,3u}){
    auto mixed=entries;mixed[1].schema=mesh_schema;mixed[2].schema=material_schema;mixed[3].schema=texture_schema;
    auto encoded=p::encode(mixed);CHECK(encoded);auto index=p::validate(encoded.value());CHECK(index&&index.value().size()==4);
    CHECK(index.value()[0].schema==1&&index.value()[1].schema==mesh_schema);
    CHECK(index.value()[2].schema==material_schema&&index.value()[3].schema==texture_schema);
    const p::Limits exact{4,2,1,encoded.value().size()};CHECK(p::encode(mixed,exact).value()==encoded.value());
    auto tighter=exact;--tighter.package_bytes;CHECK_ERROR(p::encode(mixed,tighter),ErrorCode::Exhausted);
    tighter=exact;--tighter.entries;CHECK_ERROR(p::validate(encoded.value(),tighter),ErrorCode::Exhausted);
    tighter=exact;--tighter.dependencies;CHECK_ERROR(p::validate(encoded.value(),tighter),ErrorCode::Exhausted);
    for(std::size_t i=0;i<mixed.size();++i){auto bad_schema=mixed;bad_schema[i].schema=i>=2?4:3;CHECK_ERROR(p::encode(bad_schema),ErrorCode::IncompatibleVersion);}
    mixed[0].schema=2;CHECK_ERROR(p::encode(mixed),ErrorCode::IncompatibleVersion);
  }
  auto bad=entries;bad[0].dependencies.push_back(texture_key);CHECK(!p::encode(bad));
  bad=entries;bad[2].dependencies={mesh_key};CHECK(!p::encode(bad));
  bad=entries;bad[3].dependencies={material_key};CHECK(!p::encode(bad));
  bad=entries;bad[1].dependencies={texture_key};CHECK(!p::encode(bad));
  bad=entries;bad[2].dependencies.push_back(mesh_key);CHECK(!p::encode(bad));
  bad=entries;bad[2].dependencies.clear();CHECK(p::encode(bad)); // Container admits opaque bytes; desktop enforces exact native closure.
}
void scalar_texture_schema(){
  namespace s=assets::surface;
  s::Stamp stamp;stamp.representation=s::Representation::Cooked;stamp.target=s::portable_cpu_target;
  const std::array<std::byte,1> label{std::byte{7}};stamp.source=stamp.settings=stamp.importer=stamp.cooker=assets::digest(label);
  const std::array<std::byte,12> texels{std::byte{},std::byte{2},std::byte{254},std::byte{255},std::byte{1},std::byte{3},std::byte{255},std::byte{255},
      std::byte{1},std::byte{3},std::byte{255},std::byte{255}};
  auto native=s::encode_texture_scalar_mips({90,1},stamp,2,1,s::Encoding::LinearData,texels);CHECK(native);
  p::Input input{{90,1},p::Kind::Texture,native.value(),{},3};auto encoded=p::encode({&input,1});CHECK(encoded);
  auto package=p::Package::open(encoded.value());CHECK(package&&package.value().entries().size()==1&&package.value().entries()[0].schema==3);
  const p::Key key{input.id,assets::digest(native.value())};auto lease=package.value().acquire(key);CHECK(lease);
  CHECK(std::equal(lease.value().bytes().begin(),lease.value().bytes().end(),native.value().begin(),native.value().end()));
  auto files=package.value().files();CHECK(files);auto read=files.value()->read(assets::hex(key.revision)+".rasset",native.value().size());CHECK(read&&read.value()==native.value());
  auto view=s::validate_texture(read.value());CHECK(view&&view.value().schema()==3&&view.value().encoding()==s::Encoding::LinearData&&view.value().mip_count()==2);
  for(const auto kind:{p::Kind::Mesh,p::Kind::Scene}){
    auto confused=input;confused.kind=kind;CHECK_ERROR(p::encode({&confused,1}),ErrorCode::IncompatibleVersion);
    auto bad=encoded.value();put(bad,p::header_bytes+48,static_cast<std::uint32_t>(kind),4);resign(bad);
    CHECK_ERROR(p::validate(bad),ErrorCode::IncompatibleVersion);
  }
  // Material schema3 is a supported opaque container tag. The container does
  // not reinterpret native Texture payloads as Material domain products.
  auto opaque=input;opaque.kind=p::Kind::Material;auto opaque_bytes=p::encode({&opaque,1});CHECK(opaque_bytes);
  auto opaque_package=p::Package::open(opaque_bytes.value());CHECK(opaque_package);
  auto opaque_lease=opaque_package.value().acquire(key);CHECK(opaque_lease&&opaque_lease.value().entry().kind==p::Kind::Material);
  CHECK(s::validate_texture(opaque_lease.value().bytes()));CHECK(!s::validate_material(opaque_lease.value().bytes()));
  auto tag=encoded.value();put(tag,p::header_bytes+48,static_cast<std::uint32_t>(p::Kind::Material),4);resign(tag);
  CHECK(p::validate(tag)&&tag==opaque_bytes.value());
  auto future=input;future.schema=4;CHECK_ERROR(p::encode({&future,1}),ErrorCode::IncompatibleVersion);
  future.kind=p::Kind::Material;CHECK_ERROR(p::encode({&future,1}),ErrorCode::IncompatibleVersion);
  tag=opaque_bytes.value();put(tag,p::header_bytes+52,4,4);resign(tag);CHECK_ERROR(p::validate(tag),ErrorCode::IncompatibleVersion);
  auto bad=encoded.value();put(bad,p::header_bytes+52,4,4);resign(bad);CHECK_ERROR(p::validate(bad),ErrorCode::IncompatibleVersion);
  auto dependent=input;dependent.dependencies.push_back(key);CHECK_ERROR(p::encode({&dependent,1}),ErrorCode::Unsupported);
}
void ownership(){
  auto b=golden();auto original=b;std::optional<p::Lease> lease;std::shared_ptr<platform::RootedFiles> files;
  p::Key scene_key;
  {
    auto package=p::Package::open(b);CHECK(package);CHECK(package.value().retained_bytes()==b.size());
    scene_key=package.value().entries()[0].key;lease=package.value().acquire(scene_key).value();files=package.value().files().value();
    auto missing=scene_key;++missing.id.low;CHECK_ERROR(package.value().acquire(missing),ErrorCode::NotFound);
    std::fill(b.begin(),b.end(),std::byte{});
  }
  const auto leaf=assets::hex(scene_key.revision)+".rscene";
  CHECK(std::equal(lease->bytes().begin(),lease->bytes().end(),original.begin()+336,original.begin()+341));
  CHECK(files->read(leaf,5).value()==std::vector<std::byte>(original.begin()+336,original.begin()+341));
  CHECK_ERROR(files->read(leaf,4),ErrorCode::Exhausted);
  for(const auto& invalid:{"../"+leaf,"/"+leaf,leaf+"/",leaf+"x",assets::hex(scene_key.revision)+".rasset"})CHECK_ERROR(files->read(invalid,100),ErrorCode::NotFound);
  CHECK_ERROR(files->replace(leaf,{},platform::Durability::AtomicVisibility),ErrorCode::Unsupported);
  CHECK_ERROR(files->publish_immutable(leaf,{}),ErrorCode::Unsupported);
  std::array<std::thread,4> readers;for(auto& reader:readers)reader=std::thread([files,leaf]{for(int n=0;n<100;++n){auto bytes=files->read(leaf,5);CHECK(bytes&&bytes.value()[0]==std::byte{'s'});bytes.value()[0]=std::byte{};}});
  for(auto& reader:readers)reader.join();files.reset();CHECK(lease->bytes()[0]==std::byte{'s'});lease.reset();
  auto owned=golden();const auto* storage=owned.data();auto transferred=p::Package::open_owned(std::move(owned));CHECK(transferred);
  auto retained=transferred.value().acquire(scene_key).value();CHECK(retained.bytes().data()==storage+336&&retained.bytes()[0]==std::byte{'s'});
  auto broken=golden();broken.back()^=std::byte{1};CHECK(!p::Package::open_owned(std::move(broken)));
  auto excessive=golden();excessive.reserve(excessive.size()+1);const auto capacity=excessive.capacity();
  CHECK(capacity>excessive.size());
  CHECK_ERROR(p::Package::open_owned(std::move(excessive),p::Limits{2,1,5,capacity-1}),ErrorCode::Exhausted);
  auto exact=golden();exact.reserve(exact.size()+1);const auto exact_capacity=exact.capacity();
  const auto* exact_storage=exact.data();
  auto accepted=p::Package::open_owned(std::move(exact),p::Limits{2,1,5,exact_capacity});CHECK(accepted);
  CHECK(accepted.value().acquire(scene_key).value().bytes().data()==exact_storage+336);
}
template<class F> void failures(F operation){
  bool failed=false,passed=false;
  for(std::size_t n=0;n<512;++n){asset_fail_after(n);auto result=operation();asset_fail_disable();if(result){passed=true;break;}CHECK_ERROR(result,ErrorCode::Exhausted);failed=true;}
  CHECK(failed&&passed);CHECK(operation());
}
void allocations(){
  auto b=golden();auto package=p::Package::open(b).value();auto key=package.entries()[0].key;
  std::array<p::Input,2> inputs{{{{0,1},p::Kind::Scene,std::span(b).subspan(336,5),{package.entries()[1].key}},{{0,2},p::Kind::Mesh,std::span(b).subspan(341,3),{}}}};
  failures([&]{return p::validate(b);});failures([&]{return p::encode(inputs);});failures([&]{return p::Package::open(b);});failures([&]{return package.files();});
  auto files=package.files().value();auto leaf=assets::hex(key.revision)+".rscene";failures([&]{return files->read(leaf,5);});
  // Prepare each caller-owned buffer before arming injection; only the
  // transferred snapshot's allocations belong to this API's Result contract.
  bool failed=false,passed=false;
  for(std::size_t n=0;n<64;++n){auto owned=b;asset_fail_after(n);auto result=p::Package::open_owned(std::move(owned));asset_fail_disable();if(result){passed=true;break;}CHECK_ERROR(result,ErrorCode::Exhausted);failed=true;}
  CHECK(failed&&passed);
}
}
void asset_package_tests(){wire();typed_dependencies();scalar_texture_schema();ownership();allocations();}
