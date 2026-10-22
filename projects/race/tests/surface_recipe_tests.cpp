#include "surface_recipe.hpp"
#include "assets_support.hpp"
#include "desktop_profile.hpp"
#include <algorithm>
#include <array>

namespace {
namespace r=race::surface_recipe;
namespace p=race::surface_pipeline;
namespace s=race::assets::surface;
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
assets::Digest hash(std::string_view text){return assets::digest(bytes(text));}
constexpr std::string_view mip_settings="race-rgba8-mips-1:top-left;opaque-srgb;q24-decode-nearest-half-up;area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-q24-half-up;encode-midpoint-ceil";
constexpr std::string_view mip_cooker="race-rgba8-portable-cpu-mips-cooker-1:srgb-q24-lut-1";
constexpr std::string_view scalar_settings="race-rgba8-scalar-mips-1:top-left;opaque-linear-data;independent-rgb;area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-half-up";
constexpr std::string_view scalar_cooker="race-rgba8-portable-cpu-scalar-mips-cooker-1";
struct Contract {r::Profile profile;const char* name;const char* source;const char* golden;};
constexpr std::array contracts{
  Contract{r::Profile::PpmSrgb,"ppm-srgb","source.ppm","544174e06f37b044b054dd3da7fb95b1c6ecd7921d8a9cb5959aa65ac6825ea9"},
  Contract{r::Profile::PpmLinear,"ppm-linear","source.ppm","c464e9a2c94af955b6fa465ed1d9424636b3e3ad6a992041bcf8cb0b86fc5458"},
  Contract{r::Profile::PpmSrgbMips,"ppm-srgb-mips","source.ppm","61c1ff55bea84db03af4462210086b0ee8d5555e224e9fc73eccb36b8f2eb3be"},
  Contract{r::Profile::BmpSrgb,"bmp-srgb","source.bmp","e5d554f123b0fb55696d370dde1cbfaa214ba1c2bebfda43b451c1ecbe7ddabd"},
  Contract{r::Profile::BmpLinear,"bmp-linear","source.bmp","78449e2267c0031b0c2f34f1c57acd22c78b4338f7dbc221059381530061b91a"},
  Contract{r::Profile::BmpSrgbMips,"bmp-srgb-mips","source.bmp","ccc4665954391d859291be9743a98a9be251e336ce61b9abdce994135bc8e540"},
  Contract{r::Profile::Material,"material","source.json","e039db9a261e51e9cdf38d0b1ab9c15e4177a1404c90144eb5a2394d265c3063"},
  Contract{r::Profile::MaterialMips,"material-mips","source.json","3a122ba44f438ca52df016d57369037f37ccd4304792441d7384d2aaf8a027e6"},
  Contract{r::Profile::PpmScalarMips,"ppm-scalar-mips","source.ppm","bb0b521cc51e56721d3912bd37eed34e13a3d1686958c50965678c8045ca4582"},
  Contract{r::Profile::BmpScalarMips,"bmp-scalar-mips","source.bmp","925fb072f362037ea0efb465c76095900b87dd5cb87b42dba589cf507d755b33"},
  Contract{r::Profile::MaterialScalarMips,"material-scalar-mips","source.json","84d2969651fa736450c1eeeb2fec8bd42ab673b953559ce342fdcaba0e4af929"}
};
s::Reference sample_pin(){return {aid(7),hash("texture cooked revision")};}
r::Definition definition(const Contract& c){return {aid(1),c.source,c.profile,(c.profile==r::Profile::MaterialMips||c.profile==r::Profile::MaterialScalarMips)?std::optional(sample_pin()):std::nullopt};}
// Author the complete tuple independently of profile_info/encode. Fixed Python
// hashlib vectors above additionally pin order, spelling, all hashes and final LF.
std::string expected(const Contract& c,const r::Definition& d){
  const std::string_view name=c.name;const bool material=name.starts_with("material"),mips=name.ends_with("mips"),scalar=name.ends_with("scalar-mips");
  std::string settings,importer,cooker;
  if(material){
    settings=scalar?"race-opaque-diffuse-3:linear-rgb;linear-clamp-mips;complete-cooked-linear-scalar-texture;diffuse-reflectance-r":mips?"race-opaque-diffuse-2:linear-rgb;linear-clamp-mips;complete-cooked-srgb-texture":"race-opaque-diffuse-1:linear-rgb;linear-clamp-mip0";
    importer=scalar?"race-material-json-importer-3":mips?"race-material-json-importer-2":"race-material-json-importer-1";
    cooker=scalar?"race-opaque-diffuse-portable-cpu-cooker-3:diffuse-reflectance-r;linear-clamp-mips":mips?"race-opaque-diffuse-portable-cpu-cooker-2:linear-clamp-mips":"race-opaque-diffuse-portable-cpu-cooker-1";
  }else{
    settings="race-"+std::string(name.starts_with("bmp")?"bmp":"ppm")+"-1:rgba8;top-left;"+(name.ends_with("linear")||scalar?"linear-data":"srgb")+";opaque";
    importer=name.starts_with("bmp")?"race-bmp-bgr24-info40-importer-1":"race-ppm-p6-importer-1";
    cooker=scalar?std::string(scalar_cooker):mips?std::string(mip_cooker):"race-rgba8-portable-cpu-cooker-1";
  }
  const auto cooked_settings=scalar&&!material?std::string(scalar_settings):!material&&mips?std::string(mip_settings):settings;
  std::array<std::byte,48> key{};
  if(d.texture){
    for(std::size_t i=0;i<8;++i){key[i]=std::byte((d.texture->asset.high>>(8*i))&255);key[8+i]=std::byte((d.texture->asset.low>>(8*i))&255);}
    std::copy(d.texture->revision.begin(),d.texture->revision.end(),key.begin()+16);
  }
  const auto dependency=assets::digest(d.texture?std::span<const std::byte>(key):std::span<const std::byte>{});
  return "{\"schema\":1,\"asset\":\""+assets::id_text(d.id)+"\",\"source\":\""+d.source+"\",\"profile\":\""+c.name+
    "\",\"asset_container\":1,\"asset_schema\":"+(scalar?"3":mips?"2":"1")+",\"target\":1,\"import_settings\":\""+assets::hex(hash(settings))+
    "\",\"settings\":\""+assets::hex(hash(cooked_settings))+"\",\"importer\":\""+assets::hex(hash(importer))+"\",\"cooker\":\""+assets::hex(hash(cooker))+
    "\",\"dependencies\":\""+assets::hex(dependency)+"\",\"texture_asset\":\""+assets::id_text(d.texture?d.texture->asset:assets::AssetId{})+
    "\",\"texture_revision\":\""+assets::hex(d.texture?d.texture->revision:assets::Digest{})+"\"}\n";
}
std::vector<std::string> fields(const std::string& text){
  std::vector<std::string> result;std::size_t at=1;
  for(;;){auto end=text.find_first_of(",}",at);CHECK(end!=text.npos);result.push_back(text.substr(at,end-at));if(text[end]=='}')break;at=end+1;}
  return result;
}
std::string joined(const std::vector<std::string>& items){std::string text="{";for(const auto& item:items){if(text.size()>1)text+=',';text+=item;}return text+"}";}
void canonical_contracts(){
  CHECK(static_cast<unsigned>(r::Profile::PpmScalarMips)==9&&static_cast<unsigned>(r::Profile::BmpScalarMips)==10&&static_cast<unsigned>(r::Profile::MaterialScalarMips)==11);
  for(const auto& c:contracts){
    auto d=definition(c);const auto golden=expected(c,d);CHECK(assets::hex(hash(golden))==c.golden);
    auto encoded=r::encode(d);CHECK(encoded&&encoded.value()==golden);
    CHECK(p::parse_profile(c.name).value()==c.profile);
    auto raw=golden;auto snapshot=r::decode(bytes(raw));CHECK(snapshot&&snapshot.value().definition==d);
    CHECK(snapshot.value().revision==hash(golden));std::fill(raw.begin(),raw.end(),'x');
    CHECK(snapshot.value().bytes==std::vector<std::byte>(bytes(golden).begin(),bytes(golden).end()));
    CHECK(r::encode(snapshot.value().definition).value()==golden);
    auto parts=fields(golden);CHECK(parts.size()==14);std::reverse(parts.begin(),parts.end());const auto reordered=" \r\n\t"+joined(parts)+" \n";
    auto other=r::decode(bytes(reordered));CHECK(other&&other.value().definition==d&&other.value().revision==hash(reordered));
    CHECK(other.value().revision!=snapshot.value().revision&&r::encode(other.value().definition).value()==golden);
  }
  for(auto name:{"ppm","ppm-SRGB","bmp-linear-mips","png-srgb","material ",""})CHECK_ERROR(p::parse_profile(name),ErrorCode::Unsupported);
  for(auto value:{0U,12U,255U}){auto d=definition(contracts[0]);d.profile=static_cast<r::Profile>(value);CHECK_ERROR(r::encode(d),ErrorCode::Unsupported);}
}
void strict_admission(){
  const auto text=expected(contracts[0],definition(contracts[0]));const auto original=fields(text);
  for(std::size_t index=0;index<original.size();++index){
    auto parts=original;parts.erase(parts.begin()+index);CHECK(!r::decode(bytes(joined(parts))));
    parts=original;parts.insert(parts.begin()+index,parts[index]);CHECK(!r::decode(bytes(joined(parts))));
    parts=original;parts[index]="\"unknown\":1";CHECK(!r::decode(bytes(joined(parts))));
  }
  for(std::size_t index:{0U,4U,5U,6U})for(auto value:{"0","2","-1","+1","01","1.0","1e0","4294967296","\"1\"","null","[1]"}){
    auto parts=original;parts[index]=parts[index].substr(0,parts[index].find(':')+1)+value;CHECK(!r::decode(bytes(joined(parts))));
  }
  for(std::size_t index=7;index<=11;++index){
    auto parts=original;const auto offset=parts[index].find(':')+2;parts[index][offset]=parts[index][offset]=='0'?'1':'0';
    CHECK_ERROR(r::decode(bytes(joined(parts))),ErrorCode::IncompatibleVersion);
    parts=original;parts[index][offset]='F';CHECK(!r::decode(bytes(joined(parts))));
    parts=original;parts[index].erase(offset,1);CHECK(!r::decode(bytes(joined(parts))));
  }
  for(auto bad:{"[]","null","true","{}","{\"schema\":1,}","{\"schema\":1/*comment*/}"})CHECK(!r::decode(bytes(bad)));
  for(auto suffix:{"x",",","{}", "]"})CHECK(!r::decode(bytes(text+suffix)));
  auto bad=text;bad.insert(0,"\xEF\xBB\xBF");CHECK(!r::decode(bytes(bad)));
  bad=text;bad.push_back('\0');CHECK(!r::decode(bytes(bad)));
  bad=text;bad.replace(bad.find("source.ppm"),10,"\\u0073ource.ppm");CHECK(!r::decode(bytes(bad)));
  CHECK_ERROR(r::decode({}),ErrorCode::InvalidInput);
  auto maximum=text+std::string(r::maximum_bytes-text.size(),' ');CHECK(r::decode(bytes(maximum)));
  maximum+=' ';CHECK_ERROR(r::decode(bytes(maximum)),ErrorCode::Exhausted);
  for(auto source:{"../source.ppm","SOURCE.PPM","source.bmp","a/source.ppm","source..ppm","con.ppm","a:source.ppm",".source.ppm","source.ppm ",""}){
    auto d=definition(contracts[0]);d.source=source;CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  }
  auto d=definition(contracts[0]);d.source=std::string(92,'a')+".ppm";CHECK(r::encode(d));d.source='a'+d.source;CHECK(!r::encode(d));
  d=definition(contracts[0]);d.id={};CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  d=definition(contracts[0]);d.texture=sample_pin();CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  d=definition(contracts[7]);d.texture.reset();CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  d=definition(contracts.back());d.texture.reset();CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  d=definition(contracts[7]);d.texture->asset={};CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  d=definition(contracts[7]);d.texture->revision={};CHECK_ERROR(r::encode(d),ErrorCode::InvalidInput);
  // A pin edit with an old dependency hash is rejected even when the key is valid.
  auto parts=fields(expected(contracts[7],definition(contracts[7])));auto offset=parts[13].find(':')+2;parts[13][offset]=parts[13][offset]=='0'?'1':'0';
  CHECK_ERROR(r::decode(bytes(joined(parts))),ErrorCode::IncompatibleVersion);
}
std::vector<std::byte> bmp_fixture(){
  // Independent authored BMP24/3x2 fixture (top-to-bottom expected RGB in CLI oracle).
  constexpr std::string_view hex="424d4e000000000000003600000028000000030000000200000001001800000000001800000000000000000000000000000000000000ff000080808000ffffa5a5a509200a0000ff00ff00a5a5a5";
  const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};std::vector<std::byte> result;
  for(std::size_t i=0;i<hex.size();i+=2)result.push_back(std::byte(digit(hex[i])*16+digit(hex[i+1])));return result;
}
r::Snapshot snapshot(const r::Definition& d){auto encoded=r::encode(d);CHECK(encoded);auto parsed=r::decode(bytes(encoded.value()));CHECK(parsed);return parsed.value();}
void build_and_snapshot(){
  const std::string source="P6\n# bounded header\n2 1\n255\n\n \tRGB";const auto bmp=bmp_fixture();
  r::Definition d{{4,8},"source.ppm",r::Profile::PpmSrgb,{}};auto admitted=snapshot(d);auto built=r::build(admitted,bytes(source));CHECK(built);
  CHECK(assets::hex(assets::digest(built.value().imported))=="94e699afb8d993e35435c8473a23ad6df55fe1cc081f6202e8b0a2a2c15a293e");
  CHECK(assets::hex(assets::digest(built.value().cooked))=="1126bcc83615edc6375fa6dad4c1f463adb874095cb8aaf83865902cf1bfbc89");
  CHECK(built.value().imported==p::import_ppm(d.id,bytes(source),s::Encoding::SrgbColor).value());
  CHECK(built.value().cooked==p::cook(built.value().imported).value());
  auto forged=admitted;forged.definition.id=aid(9);CHECK_ERROR(r::build(forged,bytes(source)),ErrorCode::InvalidDependency);
  forged=admitted;forged.definition.source="other.ppm";CHECK_ERROR(r::build(forged,bytes(source)),ErrorCode::InvalidDependency);
  forged=admitted;forged.definition.profile=r::Profile::PpmLinear;CHECK_ERROR(r::build(forged,bytes(source)),ErrorCode::InvalidDependency);
  forged=admitted;forged.revision[0]^=std::byte{1};CHECK_ERROR(r::build(forged,bytes(source)),ErrorCode::InvalidDependency);
  forged=admitted;forged.bytes.push_back(std::byte{' '});CHECK_ERROR(r::build(forged,bytes(source)),ErrorCode::InvalidDependency);
  forged=admitted;forged.bytes.reserve(r::maximum_bytes+1);CHECK_ERROR(r::build(forged,bytes(source)),ErrorCode::Exhausted);
  CHECK_ERROR(r::build(admitted,bmp),ErrorCode::IncompatibleVersion);CHECK_ERROR(r::build(admitted,{}),ErrorCode::InvalidInput);
  std::vector<std::byte> huge(p::source_bytes+1);CHECK_ERROR(r::build(admitted,huge),ErrorCode::Exhausted);
  for(const auto& c:contracts){
    const auto info=p::profile_info(c.profile);CHECK(info);if(info.value().material)continue;
    auto def=definition(c);def.id={4,8};auto recipe=snapshot(def);
    const std::string_view name=c.name;const bool is_bmp=name.starts_with("bmp"),scalar=name.ends_with("scalar-mips"),linear=scalar||name.ends_with("linear");
    CHECK(info.value().source_extension==(is_bmp?".bmp":".ppm"));
    const auto data=is_bmp?std::span<const std::byte>(bmp):bytes(source);auto result=r::build(recipe,data);CHECK(result);
    auto iv=s::validate_texture(result.value().imported),cv=s::validate_texture(result.value().cooked);CHECK(iv&&cv);
    CHECK(iv.value().stamp().source==assets::digest(data)&&cv.value().stamp().source==iv.value().stamp().source);
    CHECK(iv.value().schema()==1&&iv.value().encoding()==(linear?s::Encoding::LinearData:s::Encoding::SrgbColor)&&cv.value().encoding()==iv.value().encoding());
    CHECK(cv.value().schema()==(scalar?3U:info.value().mips?2U:1U));
    CHECK(result.value().imported==p::import_texture(def.id,data,iv.value().encoding()).value());
    CHECK(result.value().cooked==(scalar?p::cook_scalar_mips(result.value().imported):info.value().mips?p::cook_mips(result.value().imported):p::cook(result.value().imported)).value());
    if(scalar){
      CHECK(cv.value().mip_count()==2&&cv.value().stamp().settings==hash(scalar_settings)&&cv.value().stamp().cooker==hash(scalar_cooker));
      const auto last=cv.value().mip(1);CHECK(last&&last->width==1&&last->height==1);
      const std::array<std::byte,4> expected=is_bmp?std::array{std::byte{108},std::byte{112},std::byte{65},std::byte{255}}:
          std::array{std::byte{46},std::byte{52},std::byte{38},std::byte{255}};
      CHECK(std::equal(last->pixels.begin(),last->pixels.end(),expected.begin(),expected.end()));
    }
    if(is_bmp)CHECK_ERROR(r::build(recipe,bytes(source)),ErrorCode::IncompatibleVersion);
  }
  d.source="renamed.ppm";auto renamed=r::build(snapshot(d),bytes(source));CHECK(renamed&&renamed.value().imported==built.value().imported&&renamed.value().cooked==built.value().cooked);
  auto changed=source;changed.back()='Z';auto reimported=r::build(admitted,bytes(changed));CHECK(reimported&&reimported.value().cooked!=built.value().cooked);
  CHECK(s::validate_texture(reimported.value().cooked).value().id()==d.id);
}
std::string material_source(const std::optional<s::Reference>& pin,bool mips,bool scalar=false){
  const auto ref=pin?"{\"asset\":\""+assets::id_text(pin->asset)+"\",\"revision\":\""+assets::hex(pin->revision)+"\"}":"null";
  return "{\"schema\":"+std::string(scalar?"3":mips?"2":"1")+",\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,0.1],\"base_color_texture\":"+ref+
    (mips?",\"sampling\":\"linear-clamp-mips\"":"")+(scalar?",\"texture_role\":\"diffuse-reflectance-r\"}":"}");
}
void material_dependencies(){
  const std::string source="P6\n1 1\n255\nRGB";
  auto imported=p::import_ppm(aid(7),bytes(source),s::Encoding::SrgbColor);CHECK(imported);
  auto complete=p::cook_mips(imported.value());CHECK(complete);auto legacy=p::cook(imported.value());CHECK(legacy);
  FakeFiles files;const auto leaf=p::publish(files,complete.value());CHECK(leaf);
  const s::Reference pin{aid(7),assets::digest(complete.value())};
  r::Definition d{aid(2),"material.json",r::Profile::MaterialMips,pin};auto recipe=snapshot(d);const auto text=material_source(pin,true);
  const auto prior=files.data;const auto writes=files.writes;auto result=r::build(recipe,bytes(text),&files);CHECK(result);
  CHECK(files.data==prior&&files.writes==writes);auto view=s::validate_material(result.value().cooked);CHECK(view&&view.value().schema()==2&&view.value().material().base_color_texture==pin);
  CHECK_ERROR(r::build(recipe,bytes(text)),ErrorCode::InvalidInput);FakeFiles missing;CHECK_ERROR(r::build(recipe,bytes(text),&missing),ErrorCode::NotFound);
  CHECK_ERROR(r::build(recipe,bytes(material_source(pin,false)),&files),ErrorCode::InvalidDependency);
  auto different=pin;different.asset=aid(8);CHECK_ERROR(r::build(recipe,bytes(material_source(different,true)),&files),ErrorCode::InvalidDependency);
  different=pin;different.revision[0]^=std::byte{1};CHECK_ERROR(r::build(recipe,bytes(material_source(different,true)),&files),ErrorCode::InvalidDependency);
  FakeFiles wrong;wrong.data[leaf.value()]=legacy.value();CHECK_ERROR(r::build(recipe,bytes(text),&wrong),ErrorCode::InvalidDependency);
  for(const auto& dependency:std::array<std::vector<std::byte>,2>{imported.value(),legacy.value()}){
    const s::Reference bad{aid(7),assets::digest(dependency)};d.texture=bad;FakeFiles selected;
    selected.data[assets::hex(bad.revision)+".rasset"]=dependency;
    CHECK_ERROR(r::build(snapshot(d),bytes(material_source(bad,true)),&selected),ErrorCode::IncompatibleVersion);CHECK(selected.writes==0);
  }
  auto linear=p::import_ppm(aid(7),bytes(source),s::Encoding::LinearData);CHECK(linear);auto linear_cooked=p::cook(linear.value());CHECK(linear_cooked);
  const s::Reference bad{aid(7),assets::digest(linear_cooked.value())};d.profile=r::Profile::Material;d.texture=bad;
  FakeFiles selected;selected.data[assets::hex(bad.revision)+".rasset"]=linear_cooked.value();
  CHECK_ERROR(r::build(snapshot(d),bytes(material_source(bad,false)),&selected),ErrorCode::IncompatibleVersion);
  auto scalar=p::cook_scalar_mips(linear.value());CHECK(scalar);const s::Reference scalar_pin{aid(7),assets::digest(scalar.value())};
  selected.data[assets::hex(scalar_pin.revision)+".rasset"]=scalar.value();const auto retained=selected.data;
  for(const bool mips:{false,true}){
    d.profile=mips?r::Profile::MaterialMips:r::Profile::Material;d.texture=scalar_pin;
    CHECK_ERROR(r::build(snapshot(d),bytes(material_source(scalar_pin,mips)),&selected),ErrorCode::IncompatibleVersion);
    CHECK(selected.writes==0&&selected.data==retained);
  }
  d.profile=r::Profile::MaterialScalarMips;d.texture=scalar_pin;
  auto scalar_recipe=snapshot(d);const auto scalar_text=material_source(scalar_pin,true,true);
  auto scalar_result=r::build(scalar_recipe,bytes(scalar_text),&selected);CHECK(scalar_result);
  auto scalar_view=s::validate_material(scalar_result.value().cooked);CHECK(scalar_view&&scalar_view.value().schema()==3);
  CHECK(scalar_view.value().material().texture_role==s::TextureRole::DiffuseReflectanceR);
  CHECK(s::validate_binding(scalar_view.value(),s::validate_texture(scalar.value()).value()));
  CHECK(selected.data==retained&&selected.writes==0);
  CHECK_ERROR(r::build(scalar_recipe,bytes(material_source(scalar_pin,true)),&selected),ErrorCode::InvalidDependency);
  CHECK_ERROR(r::build(scalar_recipe,bytes(scalar_text),&missing),ErrorCode::NotFound);
  for(const auto& incompatible:std::array<std::vector<std::byte>,4>{imported.value(),legacy.value(),complete.value(),linear_cooked.value()}){
    const s::Reference foreign{aid(7),assets::digest(incompatible)};d.texture=foreign;FakeFiles dependency;
    dependency.data[assets::hex(foreign.revision)+".rasset"]=incompatible;const auto before=dependency.data;
    CHECK_ERROR(r::build(snapshot(d),bytes(material_source(foreign,true,true)),&dependency),ErrorCode::IncompatibleVersion);
    CHECK(dependency.writes==0&&dependency.data==before);
  }
  d.profile=r::Profile::Material;d.texture.reset();auto plain=r::build(snapshot(d),bytes(material_source({},false)));CHECK(plain);
  CHECK(s::validate_material(plain.value().cooked).value().material().base_color_texture==std::nullopt);
  CHECK_ERROR(r::build(snapshot(d),bytes(material_source(pin,false)),&files),ErrorCode::InvalidDependency);
  const std::string oversized(p::material_source_bytes+1,' ');CHECK_ERROR(r::build(snapshot(d),bytes(oversized)),ErrorCode::Exhausted);
}
// Arm only the operation itself; assertions and fixture verification must not
// consume the selected allocation failure. Unwinding also disarms the injector.
struct AllocationAttempt {
  explicit AllocationAttempt(std::size_t step){desktop::profile::testing::fail_allocation_after(step);}
  ~AllocationAttempt(){desktop::profile::testing::fail_next_allocation_attempts(0);}
};
template<class Operation,class Verify,class Retained>
void allocation_sweep(Operation operation,Verify verify,Retained retained){
  constexpr std::size_t limit=128;std::size_t failures{};bool recovered=false;
  for(std::size_t step=0;step<limit;++step){
    auto result=[&]{AllocationAttempt fault(step);return operation();}();
    retained();
    if(result){verify(result.value());recovered=true;break;}
    CHECK(result.error().code==ErrorCode::Exhausted);++failures;
    // Each partial failure must leave this same request usable without repair.
    auto retry=operation();CHECK(retry);verify(retry.value());retained();
  }
  CHECK(recovered&&failures>0&&failures<limit);
}
void allocation_failures(){
  CHECK(desktop::profile::allocation_counting_available());
  auto definition=::definition(contracts[7]);definition.source=std::string(91,'a')+".json";
  const auto original_definition=definition;const auto source_capacity=definition.source.capacity();
  auto encoded=expected(contracts[7],definition);const auto original_encoded=encoded;
  const auto expected_revision=hash(encoded);const std::vector<std::byte> expected_bytes(bytes(encoded).begin(),bytes(encoded).end());
  const auto retained=[&]{CHECK(definition==original_definition&&definition.source.capacity()==source_capacity&&encoded==original_encoded);};
  allocation_sweep([&]{return r::encode(definition);},[&](const std::string& value){CHECK(value==encoded);},retained);
  allocation_sweep([&]{return r::decode(bytes(encoded));},[&](const r::Snapshot& value){
    CHECK(value.definition==definition&&value.revision==expected_revision&&value.bytes==expected_bytes);
    CHECK(value.bytes.data()!=bytes(encoded).data());
  },retained);

  const std::string ppm="P6\n# bounded header\n2 1\n255\n\n \tRGB";const auto bmp=bmp_fixture();
  FakeFiles files;files.data["untouched.bin"]={std::byte{17},std::byte{31}};
  auto imported=p::import_ppm(aid(7),bytes(ppm),s::Encoding::SrgbColor);CHECK(imported);
  auto legacy=p::cook(imported.value());auto mips=p::cook_mips(imported.value());CHECK(legacy&&mips);
  const s::Reference legacy_pin{aid(7),assets::digest(legacy.value())},mip_pin{aid(7),assets::digest(mips.value())};
  files.data[assets::hex(legacy_pin.revision)+".rasset"]=legacy.value();
  files.data[assets::hex(mip_pin.revision)+".rasset"]=mips.value();
  auto linear=p::import_ppm(aid(7),bytes(ppm),s::Encoding::LinearData);CHECK(linear);
  auto scalar=p::cook_scalar_mips(linear.value());CHECK(scalar);const s::Reference scalar_pin{aid(7),assets::digest(scalar.value())};
  files.data[assets::hex(scalar_pin.revision)+".rasset"]=scalar.value();
  const auto original_files=files.data;CHECK(files.writes==0);
  const auto build_sweep=[&](const r::Definition& d,std::span<const std::byte> source){
    auto recipe=snapshot(d);const auto original_recipe=recipe;
    const auto raw_capacity=recipe.bytes.capacity(),name_capacity=recipe.definition.source.capacity();
    const std::vector<std::byte> original_source(source.begin(),source.end());
    auto built=r::build(recipe,source,&files);CHECK(built);
    const auto untouched=[&]{
      CHECK(recipe.definition==original_recipe.definition&&recipe.revision==original_recipe.revision&&recipe.bytes==original_recipe.bytes);
      CHECK(recipe.bytes.capacity()==raw_capacity&&recipe.definition.source.capacity()==name_capacity);
      CHECK(std::equal(source.begin(),source.end(),original_source.begin(),original_source.end()));
      CHECK(files.writes==0&&files.data==original_files);
    };
    const auto exact=[&](const r::Built& value){CHECK(value.imported==built.value().imported&&value.cooked==built.value().cooked);};
    allocation_sweep([&]{return r::build(recipe,source,&files);},exact,untouched);
  };
  build_sweep({aid(3),std::string(92,'p')+".ppm",r::Profile::PpmSrgbMips,{}},bytes(ppm));
  build_sweep({aid(4),std::string(92,'b')+".bmp",r::Profile::BmpSrgbMips,{}},bmp);
  build_sweep({aid(3),std::string(92,'p')+".ppm",r::Profile::PpmScalarMips,{}},bytes(ppm));
  build_sweep({aid(4),std::string(92,'b')+".bmp",r::Profile::BmpScalarMips,{}},bmp);
  for(bool mip_profile:{false,true}){
    const auto pin=mip_profile?mip_pin:legacy_pin;
    const auto material=material_source(pin,mip_profile);
    const auto reads=files.reads;
    build_sweep({aid(5),std::string(91,'m')+".json",mip_profile?r::Profile::MaterialMips:r::Profile::Material,pin},bytes(material));
    CHECK(files.reads>reads); // Both material sweeps really traverse dependency reads.
  }
  const auto scalar_source=material_source(scalar_pin,true,true);
  build_sweep({aid(5),std::string(91,'m')+".json",r::Profile::MaterialScalarMips,scalar_pin},bytes(scalar_source));
  CHECK(files.writes==0&&files.data==original_files);
}
void bounded_load(){
  const auto text=expected(contracts[0],definition(contracts[0]));FakeFiles files;files.data["recipe.json"]={bytes(text).begin(),bytes(text).end()};
  auto loaded=r::load(files,"recipe.json");CHECK(loaded&&loaded.value().revision==hash(text));
  files.data["recipe.json"].assign(10,std::byte{'x'});CHECK(loaded.value().revision==hash(text)&&r::encode(loaded.value().definition).value()==text);
  CHECK_ERROR(r::load(files,"missing.json"),ErrorCode::NotFound);
  for(auto leaf:{"../recipe.json","RECIPE.JSON","recipe.txt","sub/recipe.json","con.json"})CHECK_ERROR(r::load(files,leaf),ErrorCode::InvalidInput);
  struct Excess final:FakeFiles{Result<std::vector<std::byte>> read(std::string_view,std::size_t) override{
    auto text=expected(contracts[0],definition(contracts[0]));std::vector<std::byte> value(bytes(text).begin(),bytes(text).end());value.reserve(r::maximum_bytes+1);return value;
  }} excess;
  CHECK_ERROR(r::load(excess,"recipe.json"),ErrorCode::Exhausted);files.throw_read=true;CHECK_ERROR(r::load(files,"recipe.json"),ErrorCode::Exhausted);
}
}
int main(){canonical_contracts();strict_admission();build_and_snapshot();material_dependencies();bounded_load();allocation_failures();std::puts("versioned surface recipes passed");}
