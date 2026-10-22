// Isolated output owner qualification. Windows backing stays private; portable
// outputs reuse RACE's const AccessScope. This TU contains allocation exceptions
// before publication and is deliberately separate from exception-free SDK code.
#include "owned_records.hpp"
#include "probe_composition.hpp"
#include "race/foundation/access_scope.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>

namespace {
using namespace qualification::owned;
using race::ErrorCode;
using race::Result;
constexpr std::size_t total_bytes=64U*1024U*1024U,account_bytes=65536,output_bytes=4U*1024U*1024U;
constexpr std::size_t phase_stride=256*192+4096*288+4096,query_stride=256*144+256;
constexpr std::size_t sdk_bytes=58U*1024U*1024U,stack_bytes=1024U*1024U,reader_stack_bytes=65536;
constexpr std::size_t control_bytes=1024U*1024U-account_bytes-reader_stack_bytes;
struct Observed {
  std::size_t account_allocation{},account_regions{},output_regions{},other_regions{},
      allocations{},query_allocation{},phase_allocation{},peak_charged{},retained_charged{};
  std::atomic<std::size_t> frees{};
} observed;
void check(bool passed,const char* why) noexcept {
  if(!passed){std::fprintf(stderr,"RACE_OWNED_LEASE_REJECT: %s\n",why);std::abort();}
}
// A stateless allocator frees its own account control block without needing an
// already-destroyed accounting object. The entire 64 KiB allocation is charged.
template<class T> struct AccountAllocator {
  using value_type=T;
  AccountAllocator()=default;
  template<class U> AccountAllocator(const AccountAllocator<U>&) noexcept {}
  T* allocate(std::size_t count){
    if(count>account_bytes/sizeof(T)||alignof(T)>65536)throw std::bad_alloc{};
    auto* memory=VirtualAlloc(nullptr,account_bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!memory)throw std::bad_alloc{};
    observed.account_allocation=count*sizeof(T);++observed.account_regions;
    return static_cast<T*>(memory);
  }
  void deallocate(T* value,std::size_t) noexcept {
    check(VirtualFree(value,0,MEM_RELEASE)!=0,"account allocation release");--observed.account_regions;
  }
  template<class U> bool operator==(const AccountAllocator<U>&) const noexcept{return true;}
};
struct Account {
  std::atomic<std::size_t> live{account_bytes},peak{account_bytes};
  bool claim(std::size_t bytes) noexcept {
    auto old=live.load(std::memory_order_relaxed);
    do {if(bytes>total_bytes-old)return false;}
    while(!live.compare_exchange_weak(old,old+bytes,std::memory_order_acq_rel));
    auto high=peak.load(std::memory_order_relaxed);
    while(high<old+bytes&&!peak.compare_exchange_weak(high,old+bytes,std::memory_order_relaxed)){}
    return true;
  }
  void release(std::size_t bytes) noexcept {check(live.fetch_sub(bytes,std::memory_order_acq_rel)>=bytes,"balanced account release");}
  ~Account(){check(live.load()==account_bytes,"account outlives every charged backing");}
};
class Region {
  std::shared_ptr<Account> account_;
  void* memory_{};
  std::size_t bytes_{};
public:
  Region(std::shared_ptr<Account> account,std::size_t bytes):account_(std::move(account)),bytes_(bytes){
    if(!account_->claim(bytes_))throw std::bad_alloc{};
    memory_=VirtualAlloc(nullptr,bytes_,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!memory_){account_->release(bytes_);throw std::bad_alloc{};}
    ++observed.other_regions;
  }
  Region(const Region&)=delete;
  Region& operator=(const Region&)=delete;
  ~Region(){close();}
  std::byte* data() noexcept{return static_cast<std::byte*>(memory_);}
  void close() noexcept {
    if(!memory_)return;
    check(VirtualFree(memory_,0,MEM_RELEASE)!=0,"session region release");memory_=nullptr;
    --observed.other_regions;account_->release(bytes_);account_.reset();
  }
};
struct Slot {
  std::atomic<bool> used{};
  std::size_t requested{};
};
struct Pool {
  std::shared_ptr<Account> account;
  std::atomic<std::size_t> references{1}; // Producer plus one per live control block.
  std::array<Slot,65> slots{};
  explicit Pool(std::shared_ptr<Account> owner):account(std::move(owner)){}
  static constexpr std::size_t metadata_bytes();
  std::byte* address(std::size_t slot) noexcept {
    return reinterpret_cast<std::byte*>(this)+metadata_bytes()+(slot?phase_stride+(slot-1)*query_stride:0);
  }
  static constexpr std::size_t stride(std::size_t slot) noexcept {return slot?query_stride:phase_stride;}
  static void release(Pool* pool) noexcept {
    if(pool->references.fetch_sub(1,std::memory_order_acq_rel)!=1)return;
    auto account=std::move(pool->account);pool->~Pool();
    check(VirtualFree(pool,0,MEM_RELEASE)!=0,"output backing release");
    --observed.output_regions;account->release(output_bytes);
  }
};
constexpr std::size_t Pool::metadata_bytes(){return (sizeof(Pool)+63)&~std::size_t{63};}
static_assert(Pool::metadata_bytes()+phase_stride+64*query_stride<=output_bytes);
static_assert(std::atomic<bool>::is_always_lock_free&&std::atomic<std::size_t>::is_always_lock_free);
template<class T> struct SlotAllocator {
  using value_type=T;
  Pool* pool;
  std::size_t slot;
  SlotAllocator(Pool* p,std::size_t s) noexcept:pool(p),slot(s){}
  template<class U> SlotAllocator(const SlotAllocator<U>& other) noexcept:pool(other.pool),slot(other.slot){}
  T* allocate(std::size_t count){
    // A changed ABI fails before publication, never falls back to a heap.
    if(count!=1||sizeof(T)>Pool::stride(slot)||alignof(T)>64)throw std::bad_alloc{};
    check(pool->slots[slot].used.load()&&pool->slots[slot].requested==0,"one allocation per claimed output slot");
    pool->slots[slot].requested=sizeof(T);pool->references.fetch_add(1,std::memory_order_relaxed);
    ++observed.allocations;
    if(slot)observed.query_allocation=sizeof(T);else observed.phase_allocation=sizeof(T);
    return reinterpret_cast<T*>(pool->address(slot));
  }
  void deallocate(T* value,std::size_t count) noexcept {
    check(count==1&&reinterpret_cast<std::byte*>(value)==pool->address(slot)&&
          pool->slots[slot].requested==sizeof(T),"output control block deallocation identity");
    ++observed.frees;pool->slots[slot].requested=0;
    // Storage becomes reusable only after value and shared control destruction.
    // A producer's acquire CAS synchronizes with this release, not use_count().
    pool->slots[slot].used.store(false,std::memory_order_release);
    Pool::release(pool);
  }
  template<class U> bool operator==(const SlotAllocator<U>& other) const noexcept{return pool==other.pool&&slot==other.slot;}
};
class OutputOwner {
  Pool* pool_{};
  ResultStamp stamp_{};
  std::size_t accepted_queries_{};
  template<class T> Result<std::shared_ptr<T>> prepare(std::size_t first,std::size_t last){
    if(!pool_)return race::fail(ErrorCode::Stopping,"output owner is closed");
    for(std::size_t i=first;i<last;++i){
      bool unused=false;
      if(!pool_->slots[i].used.compare_exchange_strong(unused,true,std::memory_order_acquire))continue;
      try {
        static_assert(std::is_nothrow_default_constructible_v<T>);
        auto result=std::allocate_shared<T>(SlotAllocator<T>(pool_,i));result->stamp=stamp_;
        return result;
      }catch(const std::bad_alloc&){
        // allocate_shared returns a successfully allocated slot through its
        // allocator if construction fails. These value constructors are noexcept.
        pool_->slots[i].used.store(false,std::memory_order_release);
        return race::fail(ErrorCode::Exhausted,"output control block exceeds fixed slot");
      }
    }
    return race::fail(first?ErrorCode::Exhausted:ErrorCode::Busy,"retained output capacity is occupied");
  }
public:
  explicit OutputOwner(std::shared_ptr<Account> account){
    if(!account->claim(output_bytes))throw std::bad_alloc{};
    auto* memory=VirtualAlloc(nullptr,output_bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!memory){account->release(output_bytes);throw std::bad_alloc{};}
    pool_=::new(memory) Pool(std::move(account));++observed.output_regions;
  }
  OutputOwner(const OutputOwner&)=delete;
  OutputOwner& operator=(const OutputOwner&)=delete;
  ~OutputOwner(){close();}
  void close() noexcept {if(pool_){auto* p=pool_;pool_=nullptr;Pool::release(p);}}
  Result<void> begin_tick(ResultStamp value) noexcept {
    if(!pool_)return race::fail(ErrorCode::Stopping,"output owner is closed");
    if(value.tick<=stamp_.tick)return race::fail(ErrorCode::StaleHandle,"query tick does not advance");
    stamp_=value;accepted_queries_=0;return {};
  }
  Result<std::shared_ptr<QueryValues>> query(){
    if(!pool_)return race::fail(ErrorCode::Stopping,"output owner is closed");
    if(!stamp_.tick)return race::fail(ErrorCode::FailedPrerequisite,"query stamp is not installed");
    if(accepted_queries_==64)return race::fail(ErrorCode::Exhausted,"per-tick query limit reached");
    auto result=prepare<QueryValues>(1,65);if(result)++accepted_queries_;return result;
  }
  Result<std::shared_ptr<PhaseValues>> phase(){return prepare<PhaseValues>(0,1);}
};
using QueryLease=race::AccessScope<const QueryValues>;
using PhaseLease=race::AccessScope<const PhaseValues>;
static_assert(std::is_same_v<decltype(std::declval<QueryLease>().get()),const QueryValues&>);
struct Reader {
  std::optional<QueryLease> held;
  std::atomic<bool> release{};
};
DWORD WINAPI release_reader(void* context) noexcept {
  auto& reader=*static_cast<Reader*>(context);
  while(!reader.release.load(std::memory_order_acquire))SwitchToThread();
  check(reader.held->get().stamp.tick==1&&reader.held->get().hits[0].body.identity.incarnation==1,
        "reader retains immutable output on another thread");
  reader.held.reset();return 0;
}
} // namespace

void run_owned_leases_probe(){
  try {
    auto account=std::allocate_shared<Account>(AccountAllocator<Account>{});
    check(account->claim(stack_bytes+reader_stack_bytes),"charge owner and bounded fixture reader stacks");
    // Disposable regions exercise ownership of session backing, not execution
    // of a solver. The separate SDK probes establish its quiescent teardown.
    Region sdk(account,sdk_bytes),control(account,control_bytes);
    OutputOwner owner(account);
    check(account->live.load()==total_bytes,"all reservations including account and stack fit exactly");
    observed.peak_charged=account->peak.load();
    ResultStamp stamp;stamp.world={1,2};stamp.session={3,4};stamp.tick=1;stamp.input_generation=1;
    check(bool(owner.begin_tick(stamp)),"install first query generation");
    std::array<std::optional<QueryLease>,64> queries;
    for(std::size_t i=0;i<queries.size();++i){
      auto value=owner.query();check(bool(value),"admit all retained query slots");
      value.value()->count=256;
      for(std::size_t j=0;j<256;++j){auto& hit=value.value()->hits[j];hit.body.identity={{1,2},{3,4},i+1};hit.distance_m=double(j);}
      queries[i].emplace(std::shared_ptr<const QueryValues>(std::move(value).value()));
    }
    auto quota=owner.query();check(!quota&&quota.error().code==ErrorCode::Exhausted,"per-tick query bound");
    auto same=owner.begin_tick(stamp);check(!same&&same.error().code==ErrorCode::StaleHandle,"same tick cannot reset quota");
    ++stamp.tick;check(bool(owner.begin_tick(stamp)),"next tick resets request count only");
    auto retained=owner.query();check(!retained&&retained.error().code==ErrorCode::Exhausted,"outstanding slots survive tick advance");
    Reader reader;reader.held=queries[0];queries[0].reset();
    auto thread=CreateThread(nullptr,reader_stack_bytes,release_reader,&reader,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    check(thread!=nullptr,"create explicitly charged fixture reader");
    check(!owner.query(),"copied access scope retains the same slot");
    reader.release.store(true,std::memory_order_release);
    check(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0,"reader release quiesces before slot reuse");
    check(CloseHandle(thread)!=0,"reader handle release");account->release(reader_stack_bytes);
    auto replacement=owner.query();check(bool(replacement),"final reader release makes one slot reusable");
    check(replacement.value()->stamp.tick==2,"reused output has new stamp");
    queries[0].emplace(std::shared_ptr<const QueryValues>(std::move(replacement).value()));
    auto phase=owner.phase();check(bool(phase),"one candidate phase slot");
    phase.value()->pose_count=1;phase.value()->poses[0].body.identity={{1,2},{3,4},99};
    std::optional<PhaseLease> published;
    published.emplace(std::shared_ptr<const PhaseValues>(std::move(phase).value()));
    auto busy=owner.phase();check(!busy&&busy.error().code==ErrorCode::Busy,"phase result cannot overwrite an outstanding reader");
    published.reset();
    {auto cancelled=owner.phase();check(bool(cancelled),"prepare then cancel unpublished candidate");}
    auto final_phase=owner.phase();check(bool(final_phase),"cancel returns candidate storage");
    final_phase.value()->pose_count=1;final_phase.value()->poses[0].body.identity={{1,2},{3,4},100};
    published.emplace(std::shared_ptr<const PhaseValues>(std::move(final_phase).value()));
    owner.close();sdk.close();control.close();account->release(stack_bytes);
    auto stopped=owner.query();check(!stopped&&stopped.error().code==ErrorCode::Stopping,"stop closes output admission");
    check(observed.other_regions==0&&observed.output_regions==1,"output backing survives without session regions");
    observed.retained_charged=account->live.load();
    check(observed.retained_charged==account_bytes+output_bytes,"retained output remains fully charged");
    check(!account->claim(total_bytes-account_bytes),"new full session cannot ignore retained old output backing");
    for(std::size_t i=1;i<queries.size();++i){
      check(queries[i]->get().stamp.tick==1&&queries[i]->get().count==256,"old query generation survives stop");
      for(std::size_t j=0;j<256;++j)check(queries[i]->get().hits[j].body.identity.incarnation==i+1&&
          queries[i]->get().hits[j].distance_m==double(j),"copied hit survives session backing destruction");
    }
    check(published->get().poses[0].body.identity.incarnation==100,"immutable phase survives stop");
    for(auto& value:queries)value.reset();
    auto last=published;published.reset();account.reset();
    check(observed.account_regions==1&&observed.output_regions==1,"last phase reader owns output and accounting backing");
    last.reset();
    check(observed.account_regions==0&&observed.output_regions==0&&observed.allocations==observed.frees.load(),"last release tears down every charged owner");
    std::printf("{\"owned_leases_passed\":true,\"qualification_only\":true,\"account_control_bytes\":%zu,\"query_control_and_value_bytes\":%zu,\"phase_control_and_value_bytes\":%zu,\"pool_metadata_bytes\":%zu,\"owner_bytes\":%zu,\"access_scope_bytes\":%zu,\"control_block_allocations\":%zu,\"control_block_frees\":%zu,\"peak_charged_bytes\":%zu,\"retained_charged_bytes\":%zu,\"retained_account_regions\":%zu,\"retained_output_regions\":%zu,\"reader_stack_reservation\":%zu,\"solver_executed\":false,\"concurrent_release_tested\":true,\"allocation_proved\":false}\n",
      observed.account_allocation,observed.query_allocation,observed.phase_allocation,Pool::metadata_bytes(),sizeof(OutputOwner),sizeof(QueryLease),observed.allocations,observed.frees.load(),observed.peak_charged,observed.retained_charged,observed.account_regions,observed.output_regions,reader_stack_bytes);
  }catch(const std::bad_alloc&){check(false,"prepublication backing allocation failed");}
}

void run_composed_ownership_probe(){
  try {
    auto account=std::allocate_shared<Account>(AccountAllocator<Account>{});
    check(account->claim(stack_bytes),"charge composed owner stack");
    constexpr std::size_t composed_control_bytes=1024U*1024U-account_bytes;
    Region sdk(account,sdk_bytes),control(account,composed_control_bytes);OutputOwner owner(account);
    check(account->live.load()==total_bytes,"actual SDK output and control reservations fit exactly");
    ResultStamp stamp;stamp.world={1,2};stamp.session={3,4};stamp.tick=8;stamp.input_generation=1;stamp.origin={5,6};stamp.origin_revision=stamp.frame=1;
    stamp.producer.fill(std::byte{1});stamp.products.fill(std::byte{2});stamp.configuration.fill(std::byte{3});
    check(bool(owner.begin_tick(stamp)),"install composed candidate fixture stamp");
    auto phase=owner.phase();check(bool(phase),"reserve composed candidate phase");
    qualification::composition::Memory memory;memory.sdk=sdk.data();memory.control=control.data();memory.phase=phase.value().get();
    std::array<std::shared_ptr<QueryValues>,64> prepared;
    std::array<std::optional<QueryLease>,64> held;
    for(std::size_t i=0;i<64;++i){auto query=owner.query();check(bool(query),"reserve all composed query slots");
      prepared[i]=std::move(query).value();memory.queries[i]=prepared[i].get();}
    // The private SDK side must return only after jobs, callbacks, bodies and
    // shapes have quiesced. It receives already-reserved output storage.
    qualification::composition::execute_sdk(memory);
    check(memory.phase->pose_count==256&&!memory.phase->failed&&memory.phase->event_count>=2,"composed solved candidate values");
    for(std::size_t i=0;i<64;++i)held[i].emplace(std::shared_ptr<const QueryValues>(std::move(prepared[i])));
    std::optional<PhaseLease> result;result.emplace(std::shared_ptr<const PhaseValues>(std::move(phase).value()));
    owner.close();sdk.close();control.close();
    const auto retained=account->live.load();
    check(retained==account_bytes+output_bytes+stack_bytes,"active fixture owner stack remains charged with retained outputs");
    check(!account->claim(total_bytes-account_bytes),"composed replacement cannot bypass retained backing");
    for(const auto& value:held)check(value->get().stamp==stamp&&value->get().count==1&&
        value->get().hits[0].body.identity.object.low==129,"actual query values survive SDK and product teardown");
    check(result->get().stamp==stamp&&result->get().poses[255].body.identity.object.low==256&&
          result->get().poses[0].pose.position_m[1]>.99,"actual solved values survive all mutable owners");
    for(auto& value:held)value.reset();result.reset();
    check(observed.output_regions==0&&observed.other_regions==0&&observed.allocations==observed.frees.load(),"all composed data backing reclaimed");
    check(account->live.load()==account_bytes+stack_bytes,"only fixture owner stack and account remain");
    const auto peak=account->peak.load();account->release(stack_bytes);account.reset();
    check(observed.account_regions==0,"composed account release");
    std::printf("{\"composed_ownership_passed\":true,\"solver_executed\":true,\"bodies\":256,\"moving_bodies\":128,\"ticks\":8,\"retained_queries\":64,\"copied_hits\":64,\"copied_poses\":256,\"events\":%zu,\"pair_tests\":%zu,\"control_used_bytes\":%zu,\"control_reserved_bytes\":%zu,\"peak_charged_bytes\":%zu,\"retained_with_owner_stack_bytes\":%zu,\"sdk_peak_bytes\":%zu,\"scratch_peak_bytes\":%zu,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"whole_batch_qualified\":false,\"scene_publication_qualified\":false,\"allocation_proved\":false}\n",
        memory.events,memory.pair_tests,memory.control_used,composed_control_bytes,peak,retained,memory.sdk_peak,memory.scratch_peak);
  }catch(const std::bad_alloc&){check(false,"composed backing admission failed");}
}
