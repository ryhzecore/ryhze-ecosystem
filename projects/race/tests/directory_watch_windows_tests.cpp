#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "race/platform/directory_watch.hpp"
#include "mesh_reimport.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include "../src/platform/windows_watch_calls.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace watch=race::platform::directory_watch;
namespace fs=std::filesystem;
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::string_view triangle="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
auto bytes(std::string_view source){return std::as_bytes(std::span(source));}
watch::Snapshot changed(watch::Service& service,watch::Observation before){
  const auto deadline=Clock::now()+5s;
  for(;;){auto current=service.poll();CHECK(current&&!current.value().failure);
    if(current.value().observation!=before){CHECK(current.value().dirty);return current.value();}
    CHECK(Clock::now()<deadline);std::this_thread::sleep_for(1ms);
  }
}
void clean(watch::Service& service){
  const auto deadline=Clock::now()+5s;
  for(;;){auto current=service.poll();CHECK(current&&!current.value().failure);auto ack=service.acknowledge(current.value().observation);
    if(ack)return;CHECK(ack.error().code==ErrorCode::StaleHandle&&Clock::now()<deadline);}
}
std::unique_ptr<watch::Service> make(std::shared_ptr<platform::RootedFiles> root,std::uint64_t id,watch::Limits limits={}){
  auto result=watch::make_windows_service(std::move(root),id,limits);CHECK(result);return std::move(result).value();
}
fs::path fixture_stream_path(const fs::path &path) {
  // Extend only host stream I/O, as in platform_windows_tests. Watcher paths stay ordinary.
  auto absolute = fs::absolute(path).lexically_normal();
  absolute.make_preferred();
  const auto &native = absolute.native();
  if (native.starts_with(L"\\\\?\\"))
    return absolute;
  if (native.starts_with(L"\\\\"))
    return fs::path(L"\\\\?\\UNC\\" + native.substr(2));
  return fs::path(L"\\\\?\\" + native);
}
void print_fixture_path(const char *label, const fs::path &path) {
  // ASCII escaping preserves every UTF-16 unit even through a non-Unicode log decoder.
  std::fprintf(stderr, " %s_utf16_units=%zu %s=\"", label, path.native().size(), label);
  for (const auto unit : path.native()) {
    if (unit == L'\\' || unit == L'"')
      std::fputc('\\', stderr);
    if (unit >= 0x20 && unit <= 0x7e)
      std::fputc(static_cast<int>(unit), stderr);
    else
      std::fprintf(stderr, "\\u%04x", static_cast<unsigned>(unit));
  }
  std::fputc('"', stderr);
}
void report_stream_failure(const fs::path &path, const fs::path &host, const char *phase) {
  const DWORD windows_error = GetLastError();
  const int crt_errno = errno;
  // Cleared before each operation; these are observed thread/CRT states, not an iostream error contract.
  std::fprintf(stderr, "fixture stream failure phase=%s observed_errno=%d observed_win32_last_error=%lu",
               phase, crt_errno, static_cast<unsigned long>(windows_error));
  print_fixture_path("path", path);
  print_fixture_path("host_path", host);
  std::fputc('\n', stderr);
}
void write(const fs::path &path, std::string_view text) {
  const auto host = fixture_stream_path(path);
  errno = 0;
  SetLastError(ERROR_SUCCESS);
  std::ofstream file(host, std::ios::binary);
  if (!file)
    report_stream_failure(path, host, "open");
  CHECK(file);
  errno = 0;
  SetLastError(ERROR_SUCCESS);
  file << text;
  const bool write_failed = !file;
  if (write_failed)
    report_stream_failure(path, host, "write");
  errno = 0;
  SetLastError(ERROR_SUCCESS);
  file.close();
  if (!file && !write_failed)
    report_stream_failure(path, host, "close");
  CHECK(file);
}
thread_local unsigned arm_overflows{},arm_calls{};
thread_local bool reset_failure{};
thread_local DWORD cancel_failure{};
BOOL WINAPI injected_reset(HANDLE event){if(reset_failure){SetLastError(ERROR_INVALID_HANDLE);return FALSE;}return ResetEvent(event);}
BOOL WINAPI injected_arm(HANDLE directory,LPVOID buffer,DWORD length,BOOL subtree,DWORD filter,LPDWORD returned,LPOVERLAPPED request,LPOVERLAPPED_COMPLETION_ROUTINE callback){
  ++arm_calls;if(arm_overflows){--arm_overflows;SetLastError(ERROR_NOTIFY_ENUM_DIR);return FALSE;}
  return ReadDirectoryChangesW(directory,buffer,length,subtree,filter,returned,request,callback);
}
BOOL WINAPI injected_cancel(HANDLE directory,LPOVERLAPPED request){
  if(cancel_failure){SetLastError(cancel_failure);return FALSE;}return CancelIoEx(directory,request);
}
void controlled(const fs::path& base){
  CHECK(fs::create_directory(base/"controlled"));const auto path=base/"controlled";
  auto root=platform::make_rooted_files(path.string()).value();
  watch::detail::Calls calls{injected_reset,injected_arm,injected_cancel};
  arm_overflows=1;CHECK_ERROR(watch::detail::make_with_calls(root,200,{},calls),ErrorCode::Overflow);CHECK(!arm_overflows);
  reset_failure=true;CHECK_ERROR(watch::detail::make_with_calls(root,201,{},calls),ErrorCode::PlatformFailure);reset_failure=false;
  auto made=watch::detail::make_with_calls(root,202,{},calls);CHECK(made);auto service=std::move(made).value();
  auto before=service->poll().value().observation;write(path/"trigger.tmp","change");arm_overflows=3;
  auto observed=changed(*service,before);CHECK(observed.overflow_batches==1&&arm_overflows==2);
  auto calls_before=arm_calls;CHECK_ERROR(service->acknowledge(observed.observation),ErrorCode::StaleHandle);
  CHECK(arm_calls==calls_before+1&&arm_overflows==1);calls_before=arm_calls;
  auto second=service->poll();CHECK(second&&second.value().overflow_batches==3&&arm_overflows==0&&arm_calls==calls_before+1);
  calls_before=arm_calls;auto recovered=service->poll();CHECK(recovered&&!recovered.value().failure&&arm_calls==calls_before+1);
  clean(*service);before=service->poll().value().observation;reset_failure=true;write(path/"trigger.tmp","next");
  const auto deadline=Clock::now()+5s;
  for(;;){auto result=service->poll();CHECK(result);
    if(result.value().failure){CHECK(result.value().failure->code==ErrorCode::PlatformFailure&&result.value().dirty&&result.value().phase==watch::Phase::Stopped);break;}
    CHECK(Clock::now()<deadline);std::this_thread::sleep_for(1ms);
  }
  reset_failure=false;service.reset();
  for(auto error:{DWORD(ERROR_ACCESS_DENIED),DWORD(ERROR_NOT_FOUND)}){
    auto next=watch::detail::make_with_calls(root,203+error,{},calls);CHECK(next);service=std::move(next).value();clean(*service);
    auto weak=std::weak_ptr<platform::RootedFiles>(root);cancel_failure=error;const auto arms=arm_calls;
    auto stop=service->request_stop();if(error==ERROR_NOT_FOUND)CHECK(stop);else CHECK_ERROR(stop,ErrorCode::PlatformFailure);
    CHECK_ERROR(service->wait_stopped(0),ErrorCode::Timeout);CHECK(!weak.expired());
    // The injected cancellation did not reach Windows: the real request remains
    // live. A later real write completes it normally, even after cancellation.
    write(path/"cancel.tmp",error==ERROR_NOT_FOUND?"not-found":"denied");
    auto terminal=service->wait_stopped(5000);CHECK(terminal&&terminal.value().phase==watch::Phase::Stopped&&arm_calls==arms);
    CHECK(bool(terminal.value().failure)==(error!=ERROR_NOT_FOUND));cancel_failure=0;service.reset();
  }
  std::puts("directory immediate-overflow/reset/cancel failure controls passed");
}
void native(const fs::path& base){
  CHECK(fs::create_directory(base/"source"));auto capability=platform::make_rooted_files((base/"source").string());CHECK(capability);
  auto root=std::move(capability).value();auto service=make(root,1);auto initial=service->poll();CHECK(initial&&initial.value().dirty);
  write(base/"source"/"input.obj",triangle);auto first=changed(*service,initial.value().observation);
  CHECK_ERROR(service->acknowledge(initial.value().observation),ErrorCode::StaleHandle);clean(*service);
  for(unsigned variant=0;variant<4;++variant){
    auto before=service->poll().value().observation;
    if(variant==0)write(base/"source"/"input.obj","modified");
    if(variant==1)CHECK(root->replace("input.obj",bytes(triangle),platform::Durability::AtomicVisibility));
    if(variant==2)fs::rename(base/"source"/"input.obj",base/"source"/"renamed.obj");
    if(variant==3)CHECK(fs::remove(base/"source"/"renamed.obj"));
    auto after=changed(*service,before);CHECK(after.completed_batches>first.completed_batches);clean(*service);
  }
  std::thread foreign([&]{CHECK_ERROR(service->poll(),ErrorCode::WrongThread);CHECK_ERROR(service->request_stop(),ErrorCode::WrongThread);
    CHECK_ERROR(service->acknowledge({1,1}),ErrorCode::WrongThread);CHECK_ERROR(service->wait_stopped(0),ErrorCode::WrongThread);});foreign.join();
  CHECK_ERROR(service->wait_stopped(0),ErrorCode::Busy);CHECK_ERROR(service->wait_stopped(60001),ErrorCode::InvalidInput);
  // Fixed bounded burst exceeds the notification buffer; no expected event count
  // or filename order. Deterministic reducer tests cover overflow independently.
  for(unsigned i=0;i<1024;++i)write(base/"source"/("burst-"+std::to_string(i)+std::string(64,'x')+".tmp"),"x");
  const auto before_overflow=service->poll().value();
  for(unsigned i=0;i<32;++i){CHECK(service->poll());std::this_thread::sleep_for(1ms);}
  auto overflow=service->poll();CHECK(overflow);CHECK(overflow.value().dirty);
  std::printf("directory burst batches=%llu overflow_batches=%llu first_overflow=%llu\n",
    static_cast<unsigned long long>(overflow.value().completed_batches),static_cast<unsigned long long>(overflow.value().overflow_batches),static_cast<unsigned long long>(before_overflow.overflow_batches));
  CHECK(overflow.value().overflow_batches>0);clean(*service);
  // Warm fixed-work idle polling, one outstanding OS request throughout.
  std::array<double,9> samples{};
  for(unsigned trial=0;trial<11;++trial){auto start=Clock::now();for(unsigned i=0;i<1024;++i){auto state=service->poll();CHECK(state&&state.value().phase==watch::Phase::Watching);}
    if(trial>=2)samples[trial-2]=std::chrono::duration<double,std::micro>(Clock::now()-start).count();}
  desktop::profile::AllocationScope counted;CHECK(counted.active());
  const auto allocations=desktop::profile::allocation_snapshot();for(unsigned i=0;i<1024;++i)CHECK(service->poll());
  const auto after_allocations=desktop::profile::allocation_snapshot();
  counted.stop();CHECK(!after_allocations.overflowed&&after_allocations.requests==allocations.requests);
  std::printf("directory idle1024_us");for(double sample:samples)std::printf(" %.4f",sample);std::puts("");
  auto weak=std::weak_ptr<platform::RootedFiles>(root);root.reset();CHECK(!weak.expired());
  CHECK(service->request_stop());auto stopped=service->wait_stopped(0);if(!stopped){CHECK(stopped.error().code==ErrorCode::Timeout&&!weak.expired());stopped=service->wait_stopped(5000);}
  CHECK(stopped&&stopped.value().phase==watch::Phase::Stopped);CHECK(service->poll().value().phase==watch::Phase::Stopped);
  CHECK_ERROR(service->acknowledge(stopped.value().observation),ErrorCode::Stopping);service.reset();CHECK(weak.expired());
  // Destruction during pending/ready normal completion; no rearm after stop.
  DWORD before_cycles{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&before_cycles));
  for(unsigned i=0;i<20;++i){auto cap=platform::make_rooted_files((base/"source").string());CHECK(cap);auto w=make(cap.value(),10+i);
    auto held=std::weak_ptr<platform::RootedFiles>(cap.value());cap.value().reset();if(i%2)write(base/"source"/"race.tmp","race");
    w.reset();CHECK(held.expired());}
  DWORD after_cycles{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&after_cycles)&&after_cycles==before_cycles);
  {auto cap=platform::make_rooted_files((base/"source").string());CHECK(cap);auto limited=make(cap.value(),40,{1});
    write(base/"source"/"exhaust.tmp","x");const auto deadline=Clock::now()+5s;
    for(;;){auto state=limited->poll();CHECK(state);if(state.value().failure){CHECK(state.value().failure->code==ErrorCode::Exhausted&&state.value().dirty&&state.value().phase==watch::Phase::Stopped);break;}CHECK(Clock::now()<deadline);std::this_thread::sleep_for(1ms);}}
  CHECK_ERROR(watch::make_windows_service({},1),ErrorCode::InvalidInput);
  CHECK_ERROR(watch::make_windows_service(std::make_shared<FakeFiles>(),1),ErrorCode::Unsupported);
  {auto cap=platform::make_rooted_files((base/"source").string());CHECK(cap);
    CHECK_ERROR(watch::make_windows_service(cap.value(),0),ErrorCode::InvalidInput);
    CHECK_ERROR(watch::make_windows_service(cap.value(),50,{0}),ErrorCode::InvalidInput);
    DWORD initial_handles{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&initial_handles));std::size_t failures{};
    for(std::size_t step=0;step<32;++step){
      desktop::profile::testing::fail_allocation_after(step);auto result=watch::make_windows_service(cap.value(),60+step);
      desktop::profile::testing::fail_next_allocation_attempts(0);if(result){result.value().reset();break;}
      CHECK(result.error().code==ErrorCode::Exhausted);++failures;
      DWORD handles{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles)&&handles==initial_handles);
    }
    CHECK(failures>0&&failures<32);std::printf("directory admission allocation failure steps=%zu\n",failures);
  }
}
void reimport(const fs::path& base,const char* worker){
  CHECK(fs::create_directory(base/"authoring"));CHECK(fs::create_directory(base/"output"));
  auto root=platform::make_rooted_files((base/"authoring").string()).value();auto output=platform::make_rooted_files((base/"output").string()).value();
  write(base/"authoring"/"input.obj",triangle);
  auto watched=make(root,100);auto memory=TrackedAllocator::create({64*1024*1024}).value();
  auto made=mesh_import::Reimport::create(100,aid(1),output,memory);CHECK(made);auto owner=std::move(made).value();
  std::optional<assets::mesh::ResourceHandle> prior;
  for(unsigned iteration=0;iteration<2;++iteration){
    std::vector<std::byte> source;
    for(unsigned attempt=0;;++attempt){
      CHECK(attempt<32);auto observed=watched->poll();CHECK(observed);CHECK(owner->invalidate());
      auto read=root->read("input.obj",mesh_pipeline::Limits{}.source_bytes);CHECK(read);
      auto acknowledged=watched->acknowledge(observed.value().observation);
      if(!acknowledged){CHECK(acknowledged.error().code==ErrorCode::StaleHandle);continue;}
      source=std::move(read).value();break;
    }
    auto desired=owner->desire(assets::digest(source));CHECK(desired);
    auto stage_path=base/("import-stage-"+std::to_string(iteration));CHECK(fs::create_directory(stage_path));
    auto stage=platform::make_rooted_files(stage_path.string());CHECK(stage);auto process=platform::process::make_windows_service(100+iteration);CHECK(process);
    CHECK(owner->start(desired.value(),std::move(process).value(),stage.value(),worker,stage_path.string(),source));CHECK(owner->wait(60000));
    if(!iteration){auto current=owner->finish(desired.value());CHECK(current);prior=current.value();}
    else {
      const auto observation=watched->poll().value().observation;CHECK(fs::remove(base/"authoring"/"input.obj"));changed(*watched,observation);
      // Process notifications before admitting the completed worker. A missing
      // replacement source does not restore the old desired admission.
      CHECK(owner->invalidate());CHECK_ERROR(root->read("input.obj",1024),ErrorCode::NotFound);
      CHECK_ERROR(owner->finish(desired.value()),ErrorCode::StaleHandle);CHECK(owner->reap());CHECK(owner->current().value()==*prior);
    }
  }
}
}
int main(int argc,char** argv){
  CHECK(argc==3);const auto base=fs::absolute(argv[1]);CHECK(fs::create_directory(base));native(base);controlled(base);reimport(base,argv[2]);
  std::puts("native directory watch and missing-source reimport admission passed");
}
