#include "race/assets/package.hpp"
#include <algorithm>
#include <cstring>

namespace race::assets::pak {
namespace {
std::uint64_t get(std::span<const std::byte> b,std::size_t at,std::size_t n=8){
  std::uint64_t v{};for(std::size_t i=0;i<n;++i)v|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return v;
}
void put(std::span<std::byte> b,std::size_t at,std::uint64_t v,std::size_t n=8){
  for(std::size_t i=0;i<n;++i)b[at+i]=std::byte((v>>(8*i))&255);
}
Key key_at(std::span<const std::byte> b,std::size_t at){
  Key k{{get(b,at),get(b,at+8)},{}};std::copy_n(b.begin()+at+16,32,k.revision.begin());return k;
}
void key_put(std::span<std::byte> b,std::size_t at,const Key& k){
  put(b,at,k.id.high);put(b,at+8,k.id.low);std::copy(k.revision.begin(),k.revision.end(),b.begin()+at+16);
}
bool valid(const Key& k){return k.id.valid()&&k.revision!=Digest{};}
Result<void> envelope(std::span<const std::byte> b,const Limits& l){
  auto limits=validate_limits(l);if(!limits)return limits;
  if(b.size()>l.package_bytes)return fail(ErrorCode::Exhausted,"package byte limit");
  if(b.size()<header_bytes+entry_bytes+32||std::memcmp(b.data(),"RACEPAK1",8))return fail(ErrorCode::InvalidInput,"package signature or size");
  if(get(b,8,4)!=1||get(b,12,4)!=header_bytes||get(b,32,4)!=entry_bytes||get(b,36,4)!=dependency_bytes||get(b,56))
    return fail(ErrorCode::IncompatibleVersion,"unsupported package version or features");
  if(get(b,16)!=b.size()||get(b,40)!=header_bytes)return fail(ErrorCode::InvalidInput,"package bounds");
  const auto count=get(b,24,4),deps=get(b,28,4);
  if(!count||count>l.entries||deps>l.entries*l.dependencies)return fail(ErrorCode::Exhausted,"package index limit");
  const auto payload=header_bytes+count*entry_bytes+deps*dependency_bytes;
  if(payload>b.size()-32||get(b,48)!=payload)return fail(ErrorCode::InvalidInput,"package directory bounds");
  return {};
}
}
bool less(const Key& a,const Key& b) noexcept{
  if(a.id.high!=b.id.high)return a.id.high<b.id.high;
  if(a.id.low!=b.id.low)return a.id.low<b.id.low;
  return a.revision<b.revision;
}
Result<void> validate_limits(const Limits& l) noexcept{
  if(!l.entries||l.entries>128||l.dependencies>127||!l.chunk_bytes||l.chunk_bytes>16*1024*1024||
     l.package_bytes<header_bytes+entry_bytes+33||l.package_bytes>64*1024*1024)
    return fail(ErrorCode::InvalidInput,"invalid package limits");
  return {};
}
Result<std::vector<Entry>> validate(std::span<const std::byte> b,const Limits& l){
  auto admitted=envelope(b,l);if(!admitted)return admitted.error();
  try{
    Digest checksum;std::copy_n(b.end()-32,32,checksum.begin());
    if(digest(b.first(b.size()-32))!=checksum)return fail(ErrorCode::InvalidInput,"package checksum mismatch");
    const auto count=static_cast<std::size_t>(get(b,24,4)),deps=static_cast<std::size_t>(get(b,28,4));
    std::vector<Entry> entries;entries.reserve(count);
    std::size_t next=static_cast<std::size_t>(get(b,48)),next_dep{};
    for(std::size_t i=0;i<count;++i){
      const auto at=header_bytes+i*entry_bytes;Entry e;
      e.key=key_at(b,at);e.kind=static_cast<Kind>(get(b,at+48,4));e.schema=static_cast<std::uint32_t>(get(b,at+52,4));
      if(!valid(e.key)||(i&&!less(entries.back().key,e.key)))return fail(ErrorCode::InvalidInput,"package keys must be unique and sorted");
      if((e.kind!=Kind::Mesh&&e.kind!=Kind::Scene&&e.kind!=Kind::Texture&&e.kind!=Kind::Material)||(e.schema!=1&&!(e.schema==2&&(e.kind==Kind::Mesh||e.kind==Kind::Texture||e.kind==Kind::Material))&&!(e.schema==3&&(e.kind==Kind::Texture||e.kind==Kind::Material)))||get(b,at+56,4)!=1||get(b,at+60,4)||get(b,at+64)||get(b,at+104))
        return fail(ErrorCode::IncompatibleVersion,"unsupported package kind, schema, target, codec or chunk");
      e.offset=get(b,at+72);e.bytes=get(b,at+80);
      if(!e.bytes||e.bytes>l.chunk_bytes)return fail(ErrorCode::Exhausted,"package chunk byte limit");
      if(e.offset!=next||e.bytes>b.size()-32-next||get(b,at+88)!=e.bytes)
        return fail(ErrorCode::InvalidInput,"package overlapping, gapped or invalid chunk range");
      if(digest(b.subspan(next,static_cast<std::size_t>(e.bytes)))!=e.key.revision)return fail(ErrorCode::InvalidInput,"package chunk digest mismatch");
      next+=static_cast<std::size_t>(e.bytes);
      const auto dep_count=static_cast<std::size_t>(get(b,at+100,4));
      if(dep_count>l.dependencies)return fail(ErrorCode::Exhausted,"package dependency limit");
      if(get(b,at+96,4)!=next_dep||dep_count>deps-next_dep)return fail(ErrorCode::InvalidInput,"package dependency range");
      if((e.kind==Kind::Mesh||e.kind==Kind::Texture)&&dep_count)return fail(ErrorCode::Unsupported,"Mesh and Texture package entries have no dependencies");
      if(e.kind==Kind::Material&&dep_count>1)return fail(ErrorCode::Unsupported,"Material package profile has at most one texture");
      e.dependencies.reserve(dep_count);
      for(std::size_t d=0;d<dep_count;++d){
        auto k=key_at(b,header_bytes+count*entry_bytes+(next_dep+d)*dependency_bytes);
        if(!valid(k)||(d&&!less(e.dependencies.back(),k)))return fail(ErrorCode::InvalidInput,"package dependency keys must be unique and sorted");
        e.dependencies.push_back(k);
      }
      next_dep+=dep_count;entries.push_back(std::move(e));
    }
    if(next!=b.size()-32||next_dep!=deps)return fail(ErrorCode::InvalidInput,"package trailing data or orphan dependency records");
    for(const auto& e:entries)for(const auto& d:e.dependencies){
      auto found=std::lower_bound(entries.begin(),entries.end(),d,[](const Entry& a,const Key& k){return less(a.key,k);});
      if(found==entries.end()||found->key!=d)return fail(ErrorCode::NotFound,"package dependency missing");
      if(!((e.kind==Kind::Scene&&(found->kind==Kind::Mesh||found->kind==Kind::Material))||
           (e.kind==Kind::Material&&found->kind==Kind::Texture)))
        return fail(ErrorCode::Unsupported,"Invalid typed package dependency");
    }
    return entries;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package validation allocation");}
}
Result<std::vector<std::byte>> encode(std::span<const Input> input,const Limits& l){
  auto limits=validate_limits(l);if(!limits)return limits.error();
  if(input.empty()||input.size()>l.entries)return fail(ErrorCode::Exhausted,"package entry limit");
  try{
    std::size_t payload{},deps{};
    for(const auto& e:input){
      if(e.bytes.empty()||e.bytes.size()>l.chunk_bytes||e.dependencies.size()>l.dependencies)return fail(ErrorCode::Exhausted,"package input limit");
      payload+=e.bytes.size();deps+=e.dependencies.size();
    }
    const auto start=header_bytes+input.size()*entry_bytes+deps*dependency_bytes,size=start+payload+32;
    if(size>l.package_bytes)return fail(ErrorCode::Exhausted,"package byte limit");
    struct Item {const Input* input;Key key;};std::vector<Item> sorted;sorted.reserve(input.size());
    for(const auto& e:input)sorted.push_back({&e,{e.id,digest(e.bytes)}});
    std::sort(sorted.begin(),sorted.end(),[](const Item& a,const Item& b){return less(a.key,b.key);});
    std::vector<std::byte> b(size);std::memcpy(b.data(),"RACEPAK1",8);
    put(b,8,1,4);put(b,12,header_bytes,4);put(b,16,size);put(b,24,input.size(),4);put(b,28,deps,4);
    put(b,32,entry_bytes,4);put(b,36,dependency_bytes,4);put(b,40,header_bytes);put(b,48,start);
    std::size_t offset=start,dep{};
    for(std::size_t i=0;i<sorted.size();++i){
      const auto& item=sorted[i];const auto& e=*item.input;const auto at=header_bytes+i*entry_bytes;
      key_put(b,at,item.key);put(b,at+48,static_cast<std::uint32_t>(e.kind),4);put(b,at+52,e.schema,4);put(b,at+56,1,4);
      put(b,at+72,offset);put(b,at+80,e.bytes.size());put(b,at+88,e.bytes.size());put(b,at+96,dep,4);put(b,at+100,e.dependencies.size(),4);
      auto dependencies=e.dependencies;std::sort(dependencies.begin(),dependencies.end(),less);
      for(const auto& d:dependencies)key_put(b,header_bytes+input.size()*entry_bytes+dep++*dependency_bytes,d);
      std::copy(e.bytes.begin(),e.bytes.end(),b.begin()+offset);offset+=e.bytes.size();
    }
    auto checksum=digest(std::span<const std::byte>(b).first(size-32));std::copy(checksum.begin(),checksum.end(),b.end()-32);
    auto checked=validate(b,l);if(!checked)return checked.error();return b;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package encoding allocation");}
}
struct State {std::vector<std::byte> bytes;std::vector<Entry> entries;};
Lease::Lease(std::shared_ptr<const State> s,std::size_t i):state_(std::move(s)),index_(i){}
const Entry& Lease::entry() const noexcept{return state_->entries[index_];}
std::span<const std::byte> Lease::bytes() const noexcept{return std::span<const std::byte>(state_->bytes).subspan(static_cast<std::size_t>(entry().offset),static_cast<std::size_t>(entry().bytes));}
Package::Package(std::shared_ptr<const State> s):state_(std::move(s)){}
Result<Package> Package::open(std::span<const std::byte> b,const Limits& l){
  auto admitted=envelope(b,l);if(!admitted)return admitted.error();
  try{
    auto state=std::make_shared<State>();state->bytes.assign(b.begin(),b.end());
    auto entries=validate(state->bytes,l);if(!entries)return entries.error();state->entries=std::move(entries).value();return Package(std::move(state));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package snapshot allocation");}
}
Result<Package> Package::open_owned(std::vector<std::byte> b,const Limits& l){
  auto admitted=envelope(b,l);if(!admitted)return admitted.error();
  if(b.capacity()>l.package_bytes)return fail(ErrorCode::Exhausted,"package retained capacity limit");
  try{
    auto state=std::make_shared<State>();state->bytes=std::move(b);
    auto entries=validate(state->bytes,l);if(!entries)return entries.error();state->entries=std::move(entries).value();return Package(std::move(state));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package owned snapshot allocation");}
}
std::span<const Entry> Package::entries() const noexcept{return state_->entries;}
std::size_t Package::retained_bytes() const noexcept{return state_->bytes.size();}
Result<Lease> Package::acquire(const Key& k) const{
  const auto& entries=state_->entries;
  auto it=std::lower_bound(entries.begin(),entries.end(),k,[](const Entry& a,const Key& b){return less(a.key,b);});
  if(it==entries.end()||it->key!=k)return fail(ErrorCode::NotFound,"package chunk not found");
  return Lease(state_,static_cast<std::size_t>(it-entries.begin()));
}
namespace {
class PackageFiles final:public platform::RootedFiles {
  Package package_;
  std::vector<std::pair<std::string,Key>> names_;
public:
  explicit PackageFiles(Package p):package_(std::move(p)){
    for(const auto& e:package_.entries())names_.emplace_back(hex(e.key.revision)+(e.kind==Kind::Scene?".rscene":".rasset"),e.key);
  }
  Result<std::vector<std::byte>> read(std::string_view name,std::size_t limit) override{
    try{
      for(const auto& [leaf,key]:names_)if(name==leaf){
        auto lease=package_.acquire(key);if(!lease)return lease.error();const auto b=lease.value().bytes();
        if(b.size()>limit)return fail(ErrorCode::Exhausted,"package read byte limit");
        return std::vector<std::byte>(b.begin(),b.end());
      }
      return fail(ErrorCode::NotFound,"package canonical leaf not found");
    }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package read allocation");}
  }
  Result<void> replace(std::string_view,std::span<const std::byte>,platform::Durability) override{return fail(ErrorCode::Unsupported,"package is read only");}
  Result<void> publish_immutable(std::string_view,std::span<const std::byte>) override{return fail(ErrorCode::Unsupported,"package is read only");}
};
}
Result<std::shared_ptr<platform::RootedFiles>> Package::files() const{
  try{return std::shared_ptr<platform::RootedFiles>(std::make_shared<PackageFiles>(*this));}
  catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package file index allocation");}
}
} // namespace race::assets::pak
