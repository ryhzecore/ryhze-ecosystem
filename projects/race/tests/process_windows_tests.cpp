#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "race/platform/process.hpp"
#include "../apps/desktop_profile.hpp"
#include "test.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
using namespace race;
namespace proc=race::platform::process;
namespace {
std::uint64_t next_service_identity{1};
struct Handle {HANDLE value{};~Handle(){if(value)CloseHandle(value);}};
std::string utf8(const std::filesystem::path& path){auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
std::string text(std::span<const std::byte> bytes){return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};}
std::uint32_t integer(std::span<const std::byte> bytes,std::size_t& at){
  CHECK(at+4<=bytes.size());std::uint32_t value{};for(unsigned i=0;i<4;++i)value|=std::to_integer<std::uint32_t>(bytes[at++])<<(8*i);return value;
}
struct Fixture {
  std::string executable,directory;
  std::unique_ptr<proc::Service> service;
  Fixture(std::string exe,std::string dir,proc::Limits limits={}):executable(std::move(exe)),directory(std::move(dir)){
    auto made=proc::make_windows_service(next_service_identity++,limits);CHECK(made);service=std::move(made).value();
  }
  proc::Request start(std::span<const std::string_view> args){std::printf("child mode %.*s\n",static_cast<int>(args[0].size()),args[0].data());std::fflush(stdout);auto result=service->start({executable,directory,args});CHECK(result);return result.value();}
  proc::Completion complete(proc::Request request){auto result=service->wait(request,10000);if(!result){auto observed=service->poll(request);if(observed){const auto& v=observed.value();std::fprintf(stderr,"wait error=%u phase=%u exit=%u stdout=%llu/%zu/%d stderr=%llu/%zu/%d\n",unsigned(result.error().code),unsigned(v.phase),v.exit_code.value_or(999999),static_cast<unsigned long long>(v.stdout_observed),v.stdout_retained,int(v.stdout_finished),static_cast<unsigned long long>(v.stderr_observed),v.stderr_retained,int(v.stderr_finished));}}CHECK(result);CHECK(result.value().phase==proc::Phase::Completed);
    CHECK(result.value().stdout_finished&&result.value().stderr_finished&&result.value().exit_code);
    auto taken=service->take(request);CHECK(taken);return std::move(taken).value();}
  proc::Completion run(std::span<const std::string_view> args){return complete(start(args));}
};
void arguments(Fixture& f){
  const std::array<std::string_view,9> values{"argv","","two words","quote\"inside","trailing\\","\\\"","literal & | > < %PATH% $(no)","caf\xc3\xa9 \xe7\x8c\xab","a\\\\\"b\\"};
  auto done=f.run(values);CHECK(!done.status.failure&&*done.status.exit_code==0);
  std::size_t at{};CHECK(integer(done.out,at)==values.size()-1);
  for(std::size_t i=1;i<values.size();++i){auto size=integer(done.out,at);CHECK(at+size<=done.out.size());CHECK(text(std::span(done.out).subspan(at,size))==values[i]);at+=size;}
  CHECK(at==done.out.size());
  const auto path_units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,f.executable.data(),static_cast<int>(f.executable.size()),nullptr,0);
  CHECK(path_units>0&&path_units<8179);
  std::string exact_command(8191-static_cast<std::size_t>(path_units)-12,'x');
  const std::array<std::string_view,2> exact_args{"argv",exact_command};
  auto exact=f.run(exact_args);CHECK(!exact.status.failure);at=0;CHECK(integer(exact.out,at)==1);
  CHECK(integer(exact.out,at)==exact_command.size()&&text(std::span(exact.out).subspan(at))==exact_command);
  exact_command+='x';const std::array<std::string_view,2> oversized_command{"argv",exact_command};
  CHECK_ERROR(f.service->start({f.executable,f.directory,oversized_command}),ErrorCode::Exhausted);
  std::array<std::string_view,17> too_many{};CHECK_ERROR(f.service->start({f.executable,f.directory,too_many}),ErrorCode::Exhausted);
  const std::array<std::string_view,1> nul{std::string_view{"a\0b",3}};CHECK_ERROR(f.service->start({f.executable,f.directory,nul}),ErrorCode::InvalidInput);
  const std::array<std::string_view,1> malformed{std::string_view{"\xc0\xaf",2}};CHECK_ERROR(f.service->start({f.executable,f.directory,malformed}),ErrorCode::InvalidInput);
  CHECK_ERROR(f.service->start({"relative.exe",f.directory,{}}),ErrorCode::InvalidInput);
  CHECK_ERROR(f.service->start({f.executable+".missing",f.directory,{}}),ErrorCode::NotFound);
  std::string large(8192,'a');const std::array<std::string_view,1> oversized{large};CHECK_ERROR(f.service->start({f.executable,f.directory,oversized}),ErrorCode::Exhausted);
  desktop::profile::testing::fail_next_allocation_attempts(1);
  CHECK_ERROR(f.service->start({f.executable,f.directory,{}}),ErrorCode::Exhausted);
  desktop::profile::testing::fail_next_allocation_attempts(0);
}
void exits(Fixture& f){
  for(auto code:{"0","7","259"}){
    const std::array<std::string_view,2> args{"exit",code};auto request=f.start(args);
    CHECK_ERROR(f.service->start({f.executable,f.directory,args}),ErrorCode::Busy);
    auto done=f.service->wait(request,10000);CHECK(done&&done.value().exit_code==static_cast<std::uint32_t>(std::stoul(code)));
    CHECK_ERROR(f.service->start({f.executable,f.directory,args}),ErrorCode::Busy);
    CHECK(f.service->take(request));CHECK_ERROR(f.service->take(request),ErrorCode::StaleHandle);
  }
  const std::array<std::string_view,1> abrupt{"abrupt"};auto done=f.run(abrupt);
  CHECK(done.status.exit_code==0xc0000005U&&done.status.failure&&text(done.err)=="before abrupt termination\n");
}
void output(Fixture& f){
  const std::array<std::string_view,3> exact{"flood","65536","16384"};auto done=f.run(exact);
  CHECK(!done.status.failure&&done.out.size()==65536&&done.err.size()==16384);
  CHECK(done.status.stdout_observed==65536&&done.status.stderr_observed==16384);
  for(auto byte:done.out)CHECK(byte==std::byte{'o'});for(auto byte:done.err)CHECK(byte==std::byte{'e'});
  for(const auto& args:{std::array<std::string_view,3>{"flood","65537","0"},std::array<std::string_view,3>{"flood","0","16385"},std::array<std::string_view,3>{"flood","1048576","1048576"}}){
    done=f.run(args);CHECK(done.status.failure&&done.status.failure->code==ErrorCode::Exhausted);
    CHECK(done.out.size()<=65536&&done.err.size()<=16384);
    CHECK(done.status.stdout_observed>65536||done.status.stderr_observed>16384);
  }
}
void timeout_and_ownership(const std::string& executable,const std::string& directory){
  const auto name="Local\\race-process-gate-"+std::to_string(GetCurrentProcessId());
  Handle gate{CreateEventA(nullptr,TRUE,FALSE,name.c_str())};CHECK(gate.value);
  for(int variant=0;variant<3;++variant){
    CHECK(ResetEvent(gate.value));Fixture f(executable,directory);
    const std::array<std::string_view,2> args{"gate",name};auto request=f.start(args);
    const auto start=GetTickCount64();std::string prefix;
    while(prefix.find('\n')==prefix.npos){CHECK(GetTickCount64()-start<10000);CHECK(f.service->poll(request));prefix=text(f.service->stdout_prefix(request).value());Sleep(1);}
    CHECK(prefix.starts_with("READY "));const auto pid=static_cast<DWORD>(std::stoul(prefix.substr(6)));
    Handle child{OpenProcess(SYNCHRONIZE,FALSE,pid)};CHECK(child.value&&WaitForSingleObject(child.value,0)==WAIT_TIMEOUT);
    CHECK_ERROR(f.service->wait(request,0),ErrorCode::Timeout);CHECK_ERROR(f.service->take(request),ErrorCode::Busy);
    CHECK_ERROR(f.service->start({executable,directory,args}),ErrorCode::Busy);
    CHECK(text(f.service->stdout_prefix(request).value())==prefix&&WaitForSingleObject(child.value,0)==WAIT_TIMEOUT);
    CHECK_ERROR(f.service->poll({request.service+1,request.serial}),ErrorCode::StaleHandle);
    std::thread other([&]{CHECK_ERROR(f.service->poll(request),ErrorCode::WrongThread);
      CHECK_ERROR(f.service->request_stop(request),ErrorCode::WrongThread);
      CHECK_ERROR(f.service->start({executable,directory,args}),ErrorCode::WrongThread);});other.join();
    if(variant==0){CHECK(SetEvent(gate.value));auto done=f.complete(request);CHECK(!done.status.failure);}
    if(variant==1){CHECK(f.service->request_stop(request));auto done=f.complete(request);CHECK(done.status.failure->code==ErrorCode::Cancelled);}
    if(variant==2)f.service.reset();
    CHECK(WaitForSingleObject(child.value,0)==WAIT_OBJECT_0);
  }
}
void policies(Fixture& f){
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};Handle omitted{CreateEventW(&security,TRUE,FALSE,nullptr)};CHECK(omitted.value);
  auto value=std::to_string(reinterpret_cast<std::uintptr_t>(omitted.value));
  const std::array<std::string_view,2> handle{"handle",value};auto done=f.run(handle);
  CHECK(!done.status.failure&&text(done.out)=="BLOCKED"&&WaitForSingleObject(omitted.value,0)==WAIT_TIMEOUT);
  for(auto mode:{"deny_child","memory"}){const std::array<std::string_view,1> args{mode};done=f.run(args);CHECK(!done.status.failure&&text(done.out)=="DENIED");}
}
void admission_failures(Fixture& f){
  const std::array<std::string_view,2> args{"exit","0"};
  DWORD before{},after{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
  std::size_t failures{};
  for(std::size_t step=0;step<128;++step){
    desktop::profile::testing::fail_allocation_after(step);
    auto started=f.service->start({f.executable,f.directory,args});
    desktop::profile::testing::fail_next_allocation_attempts(0);
    if(started){CHECK(!f.complete(started.value()).status.failure);break;}
    CHECK(started.error().code==ErrorCode::Exhausted);++failures;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
  }
  CHECK(failures>10&&failures<128);
  // Existing regular file passes path admission but fails native image creation.
  const auto leaf=std::filesystem::path(f.directory)/("invalid-process-"+std::to_string(GetCurrentProcessId())+".exe");
  CHECK(!std::filesystem::exists(leaf));{std::ofstream file(leaf,std::ios::binary);file<<"not a PE executable";CHECK(file.good());}
  const auto invalid=utf8(leaf);const auto old_mode=GetThreadErrorMode();
  // Measure first-use Windows bad-image handling independently of the service.
  // This host initializes additional native handles on the first failed image.
  CHECK(SetThreadErrorMode(old_mode|SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX,nullptr));
  STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
  const bool created=CreateProcessW(leaf.c_str(),nullptr,nullptr,nullptr,FALSE,DETACHED_PROCESS,nullptr,nullptr,&startup,&child)!=FALSE;
  const auto image_error=GetLastError();
  if(created){
    (void)TerminateProcess(child.hProcess,1);
    CHECK(WaitForSingleObject(child.hProcess,INFINITE)==WAIT_OBJECT_0);
    CHECK(CloseHandle(child.hThread));CHECK(CloseHandle(child.hProcess));
  }
  CHECK(!created);std::printf("direct invalid-image error=%lu\n",image_error);std::fflush(stdout);
  CHECK(image_error==ERROR_BAD_EXE_FORMAT||image_error==ERROR_EXE_MACHINE_TYPE_MISMATCH);
  CHECK(SetThreadErrorMode(old_mode,nullptr));
  CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
  std::printf("direct native bad-image handles before=%lu after=%lu\n",before,after);before=after;
  for(unsigned i=0;i<20;++i){CHECK_ERROR(f.service->start({invalid,f.directory,args}),ErrorCode::PlatformFailure);CHECK(GetThreadErrorMode()==old_mode);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);}
  CHECK(std::filesystem::remove(leaf));CHECK(after==before);
  Fixture other(f.executable,f.directory);const auto old=f.start(args);CHECK(!f.complete(old).status.failure);
  const auto fresh=other.start(args);CHECK(old.service!=fresh.service);CHECK_ERROR(other.service->poll(old),ErrorCode::StaleHandle);
  CHECK(!other.complete(fresh).status.failure);
  proc::Limits zero;zero.stdout_bytes=zero.stderr_bytes=0;Fixture no_output(f.executable,f.directory,zero);
  CHECK(!no_output.run(args).status.failure);
  const std::array<std::string_view,3> one{"flood","1","0"};auto overflow=no_output.run(one);
  CHECK(overflow.status.failure&&overflow.status.failure->code==ErrorCode::Exhausted&&overflow.out.empty()&&overflow.status.stdout_observed==1);
  std::printf("admission allocation failures swept %zu steps; late image failure preserved handles\n",failures);
}
}
int main(int argc,char** argv){
  CHECK(argc==2);const auto executable=utf8(std::filesystem::absolute(argv[1]));const auto directory=utf8(std::filesystem::current_path());
  Fixture f(executable,directory);exits(f);arguments(f);output(f);timeout_and_ownership(executable,directory);policies(f);admission_failures(f);
  const std::array<std::string_view,2> zero{"exit","0"};DWORD before{},after{};CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
  for(int i=0;i<20;++i){auto done=f.run(zero);CHECK(!done.status.failure);}
  CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before);
  proc::Limits limits;limits.requests=1;Fixture one(executable,directory,limits);CHECK(!one.run(zero).status.failure);
  CHECK_ERROR(one.service->start({executable,directory,zero}),ErrorCode::Overflow);
  std::array<double,9> launch_us{},completion_us{};
  for(std::size_t i=0;i<launch_us.size();++i){
    const auto a=std::chrono::steady_clock::now();auto request=f.start(zero);const auto b=std::chrono::steady_clock::now();
    desktop::profile::AllocationScope scope;auto begin=desktop::profile::allocation_snapshot();
    auto completed=f.service->wait(request,10000);const auto end=desktop::profile::allocation_snapshot();scope.stop();
    const auto c=std::chrono::steady_clock::now();CHECK(completed&&!completed.value().failure&&end.requests==begin.requests);
    CHECK(f.service->take(request));launch_us[i]=std::chrono::duration<double,std::micro>(b-a).count();completion_us[i]=std::chrono::duration<double,std::micro>(c-b).count();
  }
  std::sort(launch_us.begin(),launch_us.end());std::sort(completion_us.begin(),completion_us.end());
  std::printf("zero-child nine warmed trials launch_us min=%.3f median=%.3f max=%.3f completion_us min=%.3f median=%.3f max=%.3f; zero tracked poll/wait allocations\n",launch_us.front(),launch_us[4],launch_us.back(),completion_us.front(),completion_us[4],completion_us.back());
  std::printf("owned child process tests passed; twenty reuse cycles retained handle count %lu\n",static_cast<unsigned long>(after));
}
