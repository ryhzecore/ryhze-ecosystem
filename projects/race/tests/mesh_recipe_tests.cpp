#include "mesh_recipe.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <algorithm>
#include <array>

namespace r=race::mesh_recipe;
namespace {
auto bytes(std::string_view s){return std::as_bytes(std::span(s));}
std::string encoded(mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry){auto result=r::encode({aid(1),"source.obj",profile});CHECK(result);return result.value();}
std::vector<std::string> fields(mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry){
  auto s=encoded(profile);std::vector<std::string> out;
  for(std::size_t begin=1;begin<s.size();){auto end=s.find_first_of(",}",begin);CHECK(end!=s.npos);out.push_back(s.substr(begin,end-begin));if(s[end]=='}')break;begin=end+1;}
  return out;
}
std::string join(const std::vector<std::string>& fields){std::string s="{";for(const auto& f:fields){if(s.size()>1)s+=',';s+=f;}return s+'}';}
void roundtrip(){
  auto source=encoded();auto original=source;auto result=r::decode(bytes(source));CHECK(result);
  CHECK(result.value().definition.id==aid(1)&&result.value().definition.source=="source.obj");
  CHECK(result.value().revision==assets::digest(bytes(source))&&r::encode(result.value().definition).value()==source);
  std::fill(source.begin(),source.end(),'x');CHECK(result.value().bytes==std::vector<std::byte>(bytes(original).begin(),bytes(original).end()));
  CHECK(result.value().definition.source=="source.obj"&&r::encode(result.value().definition).value()==original);
  auto f=fields();std::reverse(f.begin(),f.end());auto reordered=" \r\n\t"+join(f)+" \n";auto other=r::decode(bytes(reordered));CHECK(other);
  CHECK(other.value().revision!=result.value().revision&&r::encode(other.value().definition).value()==original);
  auto exact=original+std::string(r::maximum_bytes-original.size(),' ');CHECK(r::decode(bytes(exact)));exact+=' ';CHECK_ERROR(r::decode(bytes(exact)),ErrorCode::Exhausted);
  CHECK_ERROR(r::decode({}),ErrorCode::InvalidInput);
}
void structure(){
  const auto original=fields();CHECK(original.size()==10);
  for(std::size_t n=0;n<original.size();++n){
    auto f=original;f.erase(f.begin()+static_cast<std::ptrdiff_t>(n));CHECK(!r::decode(bytes(join(f))));
    f=original;f.insert(f.begin()+static_cast<std::ptrdiff_t>(n),f[n]);CHECK(!r::decode(bytes(join(f))));
    f=original;auto duplicated=f[n];f.erase(f.begin()+static_cast<std::ptrdiff_t>(n==0?1:0));f.insert(f.begin(),duplicated);CHECK_ERROR(r::decode(bytes(join(f))),ErrorCode::Duplicate);
    f=original;f[n]="\"unknown\":1";CHECK(!r::decode(bytes(join(f))));
  }
  for(auto malformed:{"[]","null","true","{}","{\"schema\":1,}","{\"schema\":1/*x*/}"})CHECK(!r::decode(bytes(malformed)));
  for(auto suffix:{"x",",","{}","]"})CHECK(!r::decode(bytes(encoded()+suffix)));
  auto s=encoded();s.insert(0,"\xEF");CHECK(!r::decode(bytes(s)));
  s=encoded();s.push_back('\0');CHECK(!r::decode(bytes(s)));
  s=encoded();s.replace(s.find("schema"),6,"sche\\u006da");CHECK(!r::decode(bytes(s)));
  s=encoded();s.replace(s.find("source.obj"),10,"\\u0073ource.obj");CHECK(!r::decode(bytes(s)));
}
void profile(){
  const auto original=fields();
  for(std::size_t n:{0U,3U,4U,5U}){
    for(auto value:{"0","2","-1","+1","01","1.0","1e0","4294967296","\"1\"","null","[1]"}){
      auto f=original;f[n]=f[n].substr(0,f[n].find(':')+1)+value;CHECK(!r::decode(bytes(join(f))));
    }
  }
  for(std::size_t n=6;n<10;++n){
    auto f=original;auto offset=f[n].find(':')+2;f[n][offset]=f[n][offset]=='0'?'1':'0';CHECK_ERROR(r::decode(bytes(join(f))),ErrorCode::IncompatibleVersion);
    f=original;f[n][offset]='F';CHECK(!r::decode(bytes(join(f))));
    f=original;f[n].erase(offset,1);CHECK(!r::decode(bytes(join(f))));
  }
  for(auto id:{"00000000000000000000000000000000","0000000000000000000000000000000","0000000000000000000000000000000G"}){
    auto f=original;f[1]="\"asset\":\""+std::string(id)+"\"";CHECK(!r::decode(bytes(join(f))));
  }
  for(auto source:{"../source.obj","SOURCE.OBJ","source.fbx","a/source.obj","source..obj","con.obj","a:source.obj",".source.obj","source.obj ",""}){
    auto f=original;f[2]="\"source\":\""+std::string(source)+"\"";CHECK(!r::decode(bytes(join(f))));CHECK(!r::encode({aid(1),source}));
  }
  auto longest=std::string(92,'a')+".obj";auto text=r::encode({aid(1),longest});CHECK(text&&r::decode(bytes(text.value())));CHECK(!r::encode({aid(1),"a"+longest}));
}
void uv_profile(){
  const auto text=encoded(mesh_pipeline::Profile::GeometryUv);auto decoded=r::decode(bytes(text));CHECK(decoded);
  // Independent canonical byte vectors, including the original schema 1 newline.
  CHECK(assets::hex(assets::digest(bytes(encoded())))=="08416ada381c3697e02aed04b384c5bef55027948ee0834e5595905793b2fbd6");
  CHECK(assets::hex(assets::digest(bytes(text)))=="477a24ad21724e7d99242e114f06c7eec314368baf686791259c121ab6e18320");
  CHECK(decoded.value().definition.profile==mesh_pipeline::Profile::GeometryUv&&r::encode(decoded.value().definition).value()==text);
  auto reordered=fields(mesh_pipeline::Profile::GeometryUv);std::reverse(reordered.begin(),reordered.end());
  auto other=r::decode(bytes(join(reordered)));CHECK(other&&other.value().definition.profile==mesh_pipeline::Profile::GeometryUv);
  CHECK(r::encode(other.value().definition).value()==text&&other.value().revision!=decoded.value().revision);
  const auto legacy=fields(),uv=fields(mesh_pipeline::Profile::GeometryUv);
  // Every differing tuple element rejects when transplanted into either profile.
  for(std::size_t n:{0U,4U,6U,7U,8U}){
    auto cross=uv;cross[n]=legacy[n];CHECK_ERROR(r::decode(bytes(join(cross))),ErrorCode::IncompatibleVersion);
    cross=legacy;cross[n]=uv[n];CHECK_ERROR(r::decode(bytes(join(cross))),ErrorCode::IncompatibleVersion);
  }
  for(auto value:{0U,3U,255U})CHECK_ERROR(r::encode({aid(1),"source.obj",static_cast<mesh_pipeline::Profile>(value)}),ErrorCode::IncompatibleVersion);
  std::size_t failures{};
  for(std::size_t n=0;n<16;++n){desktop::profile::testing::fail_allocation_after(n);auto result=r::decode(bytes(text));desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;CHECK(result.error().code==ErrorCode::Exhausted);CHECK(r::decode(bytes(text)));++failures;}
  CHECK(failures>0&&failures<16);
}
void files_and_faults(){
  struct Reserved final:FakeFiles {
    Result<std::vector<std::byte>> read(std::string_view,std::size_t) override {
      auto s=encoded();std::vector<std::byte> b(bytes(s).begin(),bytes(s).end());b.reserve(r::maximum_bytes+1);return b;
    }
  } reserved;
  CHECK_ERROR(r::load(reserved,"recipe.json"),ErrorCode::Exhausted);
  FakeFiles files;auto text=encoded();files.data["recipe.json"]={bytes(text).begin(),bytes(text).end()};auto result=r::load(files,"recipe.json");CHECK(result);
  CHECK_ERROR(r::load(files,"missing.json"),ErrorCode::NotFound);CHECK(!r::load(files,"../recipe.json"));CHECK(!r::load(files,"source.obj"));
  files.data["recipe.json"].push_back(std::byte{1});CHECK(!r::load(files,"recipe.json"));CHECK(result.value().revision==assets::digest(bytes(text)));
  std::size_t failures{};
  for(std::size_t n=0;n<16;++n){
    desktop::profile::testing::fail_allocation_after(n);auto parsed=r::decode(bytes(text));desktop::profile::testing::fail_next_allocation_attempts(0);
    if(parsed)break;CHECK(parsed.error().code==ErrorCode::Exhausted);++failures;
  }
  CHECK(failures>0&&failures<16);failures=0;
  const r::Definition input{aid(1),"source.obj"};
  for(std::size_t n=0;n<64;++n){
    desktop::profile::testing::fail_allocation_after(n);auto parsed=r::encode(input);desktop::profile::testing::fail_next_allocation_attempts(0);
    if(parsed)break;CHECK(parsed.error().code==ErrorCode::Exhausted);++failures;
  }
  CHECK(failures>2&&failures<64);files.throw_read=true;CHECK_ERROR(r::load(files,"recipe.json"),ErrorCode::Exhausted);
}
void measured(){
  const auto text=encoded();std::array<double,9> times{};std::size_t allocations{},peak{};
  for(unsigned trial=0;trial<11;++trial){
    const auto start=std::chrono::steady_clock::now();
    for(unsigned n=0;n<1024;++n){auto item=r::decode(bytes(text));CHECK(item&&item.value().definition.id==aid(1));peak=std::max(peak,item.value().bytes.capacity()+item.value().definition.source.capacity());}
    const auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();if(trial>=2)times[trial-2]=elapsed;
  }
  {desktop::profile::AllocationScope scope;const auto before=desktop::profile::allocation_snapshot();for(unsigned n=0;n<1024;++n){auto item=r::decode(bytes(text));CHECK(item);}
    const auto after=desktop::profile::allocation_snapshot();CHECK(!after.overflowed);allocations=static_cast<std::size_t>(after.requests-before.requests);}
  auto sorted=times;std::sort(sorted.begin(),sorted.end());std::printf("recipe bytes=%zu peak_owned_capacity=%zu 1024_decode_allocations=%zu median_us=%.4f samples_us=",text.size(),peak,allocations,sorted[4]);
  for(auto sample:times)std::printf("%.4f,",sample);std::puts("");
}
}
int main(){roundtrip();structure();profile();uv_profile();files_and_faults();measured();std::puts("versioned mesh recipe passed");}
