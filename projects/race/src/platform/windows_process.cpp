#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "race/platform/process.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <string>
#include <thread>

namespace race::platform::process {
namespace {
struct Handle {
  HANDLE value{};
  Handle()=default;
  explicit Handle(HANDLE v):value(v==INVALID_HANDLE_VALUE?nullptr:v){}
  ~Handle(){if(value)CloseHandle(value);}
  Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
  void reset(HANDLE v=nullptr) noexcept {if(value)CloseHandle(value);value=v==INVALID_HANDLE_VALUE?nullptr:v;}
};
Result<std::wstring> wide(std::string_view text,bool absolute=false){
  if(text.size()>16384||text.find('\0')!=text.npos)return fail(ErrorCode::InvalidInput,"Invalid process UTF-8 argument length or NUL");
  if(absolute&&(text.size()<3||!((text[0]>='A'&&text[0]<='Z')||(text[0]>='a'&&text[0]<='z'))||
      text[1]!=':'||(text[2]!='/'&&text[2]!='\\')))return fail(ErrorCode::InvalidInput,"Process path must be absolute local drive path");
  if(text.empty())return std::wstring{};
  const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
  if(!size)return fail(ErrorCode::InvalidInput,"Invalid process UTF-8");
  std::wstring out(static_cast<std::size_t>(size),L'\0');
  if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),size)!=size)
    return fail(ErrorCode::PlatformFailure,"Process UTF-8 conversion failed");
  return out;
}
void quote(std::wstring& command,std::wstring_view argument){
  if(!command.empty())command+=L' ';command+=L'"';std::size_t slashes=0;
  for(auto c:argument){
    if(c==L'\\'){++slashes;continue;}
    command.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;command+=c;
  }
  command.append(slashes*2,L'\\');command+=L'"';
}
struct Attributes {
  std::vector<std::byte> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST list{};
  ~Attributes(){if(list)DeleteProcThreadAttributeList(list);}
  Result<void> initialize(){
    SIZE_T bytes{};(void)InitializeProcThreadAttributeList(nullptr,3,0,&bytes);
    if(!bytes||bytes>65536)return fail(ErrorCode::PlatformFailure,"Process attribute storage query failed");
    storage.resize(bytes);auto* pointer=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(pointer,3,0,&bytes))return fail(ErrorCode::PlatformFailure,"Process attributes unavailable");
    list=pointer;return {};
  }
};
struct ErrorMode {
  DWORD previous{};
  bool active{};
  ~ErrorMode(){if(active&&!SetThreadErrorMode(previous,nullptr))std::terminate();}
  bool initialize() noexcept {
    active=SetThreadErrorMode(GetThreadErrorMode()|SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX,&previous)!=FALSE;
    return active;
  }
};
std::atomic<std::uint64_t> pipe_serial{1};
struct Pipe {
  Handle read,write,event;
  OVERLAPPED operation{};
  std::array<std::byte,4096> pending_bytes{};
  std::vector<std::byte> retained;
  std::size_t size{};
  std::uint64_t observed{};
  bool pending{},finished{};
  ~Pipe(){
    if(pending){
      (void)CancelIoEx(read.value,&operation);DWORD bytes{};
      if(!GetOverlappedResult(read.value,&operation,&bytes,TRUE)&&GetLastError()==ERROR_IO_INCOMPLETE)std::terminate();
    }
  }
  Result<void> initialize(std::size_t limit){
    retained.resize(limit);
    if(retained.capacity()>limit)return fail(ErrorCode::Exhausted,"Process capture allocated capacity exceeds limit");
    event.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event.value)return fail(ErrorCode::PlatformFailure,"Process pipe event creation failed");
    operation.hEvent=event.value;
    // Names are process-local monotonic identities. First-instance admission and
    // rejecting remote clients prevent accidental endpoint reuse; no sandbox claim.
    auto serial=pipe_serial.load(std::memory_order_relaxed);
    for(;;){
      if(serial==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Process pipe identities exhausted");
      if(pipe_serial.compare_exchange_weak(serial,serial+1,std::memory_order_relaxed))break;
    }
    const auto name=L"\\\\.\\pipe\\race-owned-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(serial);
    read.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));
    if(!read.value)return fail(ErrorCode::PlatformFailure,"Process output pipe creation failed");
    if(!ConnectNamedPipe(read.value,&operation)){
      const auto error=GetLastError();
      if(error==ERROR_IO_PENDING)pending=true;
      else if(error!=ERROR_PIPE_CONNECTED)return fail(ErrorCode::PlatformFailure,"Process pipe connection failed");
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    write.reset(CreateFileW(name.c_str(),GENERIC_WRITE,0,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!write.value)return fail(ErrorCode::PlatformFailure,"Process pipe writer open failed");
    if(pending){DWORD count{};const bool connected=GetOverlappedResult(read.value,&operation,&count,TRUE)!=FALSE;
      pending=false;if(!connected)return fail(ErrorCode::PlatformFailure,"Process pipe connection completion failed");}
    return {};
  }
  // At most four completed reads per stream per poll. Further bytes remain in
  // the pipe; overflow retains its prefix and asks the service to terminate.
  std::optional<Error> drain() noexcept {
    std::optional<Error> failure;
    for(unsigned work=0;work<4&&!finished;++work){
      DWORD count{};bool ready=false;
      if(pending){
        ready=GetOverlappedResult(read.value,&operation,&count,FALSE)!=FALSE;
        if(!ready&&GetLastError()==ERROR_IO_INCOMPLETE)break;
        pending=false;
      }else{
        if(!ResetEvent(event.value)){finished=true;return fail(ErrorCode::IoFailure,"Process read event reset failed");}
        ready=ReadFile(read.value,pending_bytes.data(),static_cast<DWORD>(pending_bytes.size()),&count,&operation)!=FALSE;
        if(!ready&&GetLastError()==ERROR_IO_PENDING){pending=true;break;}
      }
      if(!ready){
        const auto error=GetLastError();finished=true;
        if(error!=ERROR_BROKEN_PIPE&&error!=ERROR_PIPE_NOT_CONNECTED)
          failure=fail(ErrorCode::IoFailure,"Process output read failed");
        break;
      }
      if(count>pending_bytes.size()){finished=true;return fail(ErrorCode::Invariant,"Process read count exceeds buffer");}
      if(observed>std::numeric_limits<std::uint64_t>::max()-count){
        failure=fail(ErrorCode::Overflow,"Process output counter exhausted");observed=std::numeric_limits<std::uint64_t>::max();
      }else observed+=count;
      const auto keep=std::min<std::size_t>(count,retained.size()-size);
      if(keep)std::copy_n(pending_bytes.data(),keep,retained.data()+size);
      size+=keep;
      if(keep<count&&!failure)failure=fail(ErrorCode::Exhausted,"Process output limit exceeded");
    }
    return failure;
  }
};
struct Session {
  Handle job,process;
  Pipe out,err;
  std::wstring executable,directory,command,environment;
  Snapshot status;
  bool process_done{};
};
class WindowsService final : public Service {
  const std::thread::id owner_{std::this_thread::get_id()};
  const std::uint64_t identity_;
  const Limits limits_;
  std::uint64_t serial_{};
  std::unique_ptr<Session> session_;
  Result<void> valid(Request request) const noexcept {
    if(owner_!=std::this_thread::get_id())return fail(ErrorCode::WrongThread,"Process operation requires owner thread");
    if(!session_||session_->status.request!=request)return fail(ErrorCode::StaleHandle,"Process request is stale");
    return {};
  }
  Result<void> terminate(Error reason) noexcept {
    auto& s=*session_;
    if(s.status.phase==Phase::Completed)return {};
    if(!s.status.failure)s.status.failure=reason;
    if(!s.process_done&&!TerminateJobObject(s.job.value,1))return fail(ErrorCode::PlatformFailure,"Process termination request failed; ownership retained");
    s.status.phase=Phase::Stopping;return {};
  }
public:
  WindowsService(std::uint64_t identity,Limits limits):identity_(identity),limits_(limits){}
  ~WindowsService() override {
    if(!session_)return;
    if(owner_!=std::this_thread::get_id())std::terminate();
    const auto request=session_->status.request;
    if(session_->status.phase!=Phase::Completed){
      if(!terminate(fail(ErrorCode::Cancelled,"Process owner shutdown")))std::terminate();
      for(;;){auto observed=poll(request);if(!observed)std::terminate();
        if(observed.value().phase==Phase::Completed)break;Sleep(1);}
    }
  }
  Result<Request> start(const Launch& launch) override {
    if(owner_!=std::this_thread::get_id())return fail(ErrorCode::WrongThread,"Process start requires owner thread");
    if(session_)return fail(ErrorCode::Busy,"Process slot retains a child or completion");
    if(serial_==limits_.requests)return fail(ErrorCode::Overflow,"Process request identities exhausted");
    if(launch.arguments.size()>16)return fail(ErrorCode::Exhausted,"Process argument count exceeds limit");
    try{
      auto executable=wide(launch.executable,true);if(!executable)return executable.error();
      auto directory=wide(launch.working_directory,true);if(!directory)return directory.error();
      auto s=std::make_unique<Session>();s->executable=std::move(executable).value();s->directory=std::move(directory).value();
      const auto file=GetFileAttributesW(s->executable.c_str()),folder=GetFileAttributesW(s->directory.c_str());
      if(file==INVALID_FILE_ATTRIBUTES)return fail(ErrorCode::NotFound,"Process executable not found");
      if((file&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||folder==INVALID_FILE_ATTRIBUTES||
          !(folder&FILE_ATTRIBUTE_DIRECTORY)||(folder&FILE_ATTRIBUTE_REPARSE_POINT))return fail(ErrorCode::InvalidInput,"Process executable or working directory invalid");
      quote(s->command,s->executable);
      for(auto value:launch.arguments){auto converted=wide(value);if(!converted)return converted.error();quote(s->command,converted.value());
        if(s->command.size()>8191)return fail(ErrorCode::Exhausted,"Process encoded command line exceeds 16 KiB");}
      if(s->command.size()>8191)return fail(ErrorCode::Exhausted,"Process encoded command line exceeds 16 KiB");
      const auto win_size=GetWindowsDirectoryW(nullptr,0);
      if(!win_size||win_size>32768)return fail(ErrorCode::PlatformFailure,"Process system directory query failed");
      std::wstring windows(win_size,L'\0');const auto used=GetWindowsDirectoryW(windows.data(),win_size);
      if(!used||used>=win_size)return fail(ErrorCode::PlatformFailure,"Process system directory changed");windows.resize(used);
      s->environment=L"PATH="+windows+L"\\System32";s->environment+=L'\0';
      s->environment+=L"SystemRoot="+windows;s->environment+=L'\0';s->environment+=L'\0';
      auto ready=s->out.initialize(limits_.stdout_bytes);if(!ready)return ready.error();
      ready=s->err.initialize(limits_.stderr_bytes);if(!ready)return ready.error();
      s->job.reset(CreateJobObjectW(nullptr,nullptr));if(!s->job.value)return fail(ErrorCode::PlatformFailure,"Process job creation failed");
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
      job_limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      job_limits.BasicLimitInformation.ActiveProcessLimit=1;job_limits.ProcessMemoryLimit=256U*1024U*1024U;
      if(!SetInformationJobObject(s->job.value,JobObjectExtendedLimitInformation,&job_limits,sizeof(job_limits)))
        return fail(ErrorCode::PlatformFailure,"Process job limits unavailable");
      SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
      Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
      if(!input.value)return fail(ErrorCode::PlatformFailure,"Process stdin creation failed");
      Attributes attributes;ready=attributes.initialize();if(!ready)return ready.error();
      std::array<HANDLE,3> inherited{input.value,s->out.write.value,s->err.write.value};
      DWORD child_policy=PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
      if(!UpdateProcThreadAttribute(attributes.list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited.data(),sizeof(inherited),nullptr,nullptr)||
          !UpdateProcThreadAttribute(attributes.list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&s->job.value,sizeof(HANDLE),nullptr,nullptr)||
          !UpdateProcThreadAttribute(attributes.list,0,PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,&child_policy,sizeof(child_policy),nullptr,nullptr))
        return fail(ErrorCode::Unsupported,"Required process inheritance/job/child policy unavailable");
      STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
      startup.StartupInfo.hStdInput=input.value;startup.StartupInfo.hStdOutput=s->out.write.value;startup.StartupInfo.hStdError=s->err.write.value;
      startup.lpAttributeList=attributes.list;PROCESS_INFORMATION created{};
      // Console-free child with explicit streams. CREATE_NO_WINDOW failed during
      // DLL initialization under this job/child policy on the validated host.
      // DETACHED_PROCESS concerns the console, not job or lifetime ownership.
      ErrorMode error_mode;
      if(!error_mode.initialize())return fail(ErrorCode::PlatformFailure,"Process launch error-mode admission failed");
      if(!CreateProcessW(s->executable.c_str(),s->command.data(),nullptr,nullptr,TRUE,
          DETACHED_PROCESS|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,s->environment.data(),s->directory.c_str(),&startup.StartupInfo,&created))
        return fail(ErrorCode::PlatformFailure,"Process creation failed with required job and policy");
      // All allocation/launch preparation precedes this point. Establish stable
      // ownership before any polling; thread and parent pipe writers close now.
      s->process.reset(created.hProcess);Handle thread(created.hThread);
      s->out.write.reset();s->err.write.reset();s->status.request={identity_,++serial_};
      const auto request=s->status.request;session_=std::move(s);return request;
    }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Process admission allocation failed");}
  }
  Result<Snapshot> poll(Request request) noexcept override {
    auto checked=valid(request);if(!checked)return checked.error();auto& s=*session_;
    if(s.status.phase==Phase::Completed)return s.status;
    for(auto* stream:{&s.out,&s.err})if(auto error=stream->drain()){
      auto stopped=terminate(*error);if(!stopped)return stopped.error();
    }
    const auto signaled=WaitForSingleObject(s.process.value,0);
    if(signaled==WAIT_FAILED)return fail(ErrorCode::PlatformFailure,"Process signal observation failed; ownership retained");
    if(signaled==WAIT_OBJECT_0){
      s.process_done=true;
      if(!s.status.exit_code){DWORD code{};if(!GetExitCodeProcess(s.process.value,&code))return fail(ErrorCode::PlatformFailure,"Process exit-code query failed; ownership retained");s.status.exit_code=code;}
    }
    s.status.stdout_observed=s.out.observed;s.status.stderr_observed=s.err.observed;
    s.status.stdout_retained=s.out.size;s.status.stderr_retained=s.err.size;
    s.status.stdout_finished=s.out.finished;s.status.stderr_finished=s.err.finished;
    if(s.process_done&&s.out.finished&&s.err.finished){
      if(*s.status.exit_code&&!s.status.failure)s.status.failure=fail(ErrorCode::PlatformFailure,"Child exited unsuccessfully");
      s.status.phase=Phase::Completed;
    }
    return s.status;
  }
  Result<std::span<const std::byte>> stdout_prefix(Request request) const noexcept override {
    auto checked=valid(request);if(!checked)return checked.error();return std::span<const std::byte>{session_->out.retained.data(),session_->out.size};
  }
  Result<std::span<const std::byte>> stderr_prefix(Request request) const noexcept override {
    auto checked=valid(request);if(!checked)return checked.error();return std::span<const std::byte>{session_->err.retained.data(),session_->err.size};
  }
  Result<void> request_stop(Request request) noexcept override {
    auto checked=valid(request);if(!checked)return checked.error();return terminate(fail(ErrorCode::Cancelled,"Process cancellation requested"));
  }
  Result<Snapshot> wait(Request request,std::uint32_t milliseconds) noexcept override {
    auto checked=valid(request);if(!checked)return checked.error();
    if(milliseconds>60000)return fail(ErrorCode::InvalidInput,"Process wait exceeds per-call limit");
    const auto begin=GetTickCount64();
    for(;;){
      auto observed=poll(request);if(!observed)return observed.error();
      if(observed.value().phase==Phase::Completed)return observed;
      if(GetTickCount64()-begin>=milliseconds)return fail(ErrorCode::Timeout,"Process completion wait timed out; ownership retained");
      Sleep(1);
    }
  }
  Result<Completion> take(Request request) noexcept override {
    auto checked=valid(request);if(!checked)return checked.error();auto& s=*session_;
    if(s.status.phase!=Phase::Completed)return fail(ErrorCode::Busy,"Process and streams have not reached quiescence");
    s.out.retained.resize(s.out.size);s.err.retained.resize(s.err.size);
    Completion complete{s.status,std::move(s.out.retained),std::move(s.err.retained)};
    session_.reset();return complete;
  }
};
}
Result<std::unique_ptr<Service>> make_windows_service(std::uint64_t identity,const Limits& limits){
  if(!identity||limits.stdout_bytes>65536||limits.stderr_bytes>16384||!limits.requests)
    return fail(ErrorCode::InvalidInput,"Invalid process service limits or identity");
  try{return std::unique_ptr<Service>{std::make_unique<WindowsService>(identity,limits)};}
  catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Process service allocation failed");}
}
} // namespace race::platform::process
