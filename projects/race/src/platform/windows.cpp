#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
#include "race/platform/rooted_files.hpp"
#include "windows_root_access.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <thread>

namespace race::platform {
namespace {
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  bool discard{false};
  explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  Handle(Handle&& h) noexcept:value(h.value),discard(h.discard){h.value=INVALID_HANDLE_VALUE;}
  ~Handle(){if(value!=INVALID_HANDLE_VALUE) {
    if(discard){FILE_DISPOSITION_INFO info{TRUE};SetFileInformationByHandle(value,FileDispositionInfo,&info,sizeof(info));}
    CloseHandle(value);
  }}
  bool valid() const {return value!=INVALID_HANDLE_VALUE;}
};
Result<std::wstring> native_path(std::string_view path) {
  if(path.size()<3 || path.size()>32000 || path.find('\0')!=path.npos ||
     !((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z')) ||
     path[1]!=':' || (path[2]!='/'&&path[2]!='\\'))
    return fail(ErrorCode::InvalidInput,"absolute drive path required");
  const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),static_cast<int>(path.size()),nullptr,0);
  if(!n) return fail(ErrorCode::InvalidInput,"invalid UTF-8 path");
  std::wstring out(static_cast<std::size_t>(n),L'\0');
  MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.data(),static_cast<int>(path.size()),out.data(),n);
  std::replace(out.begin(),out.end(),L'/',L'\\');
  for(std::size_t begin=3;begin<out.size();) {
    auto end=out.find(L'\\',begin); if(end==out.npos)end=out.size();
    auto leaf=out.substr(begin,end-begin);
    if(leaf.empty() || leaf==L"." || leaf==L".." || leaf.find(L':')!=leaf.npos ||
       leaf.back()==L'.' || leaf.back()==L' ')
      return fail(ErrorCode::InvalidInput,"noncanonical Windows path");
    for(wchar_t c:leaf)if(c<32||c==L'<'||c==L'>'||c==L'"'||c==L'|'||c==L'?'||c==L'*')
      return fail(ErrorCode::InvalidInput,"invalid Windows path character");
    auto base=leaf.substr(0,leaf.find(L'.'));
    for(auto& c:base)if(c>=L'a'&&c<=L'z')c-=L'a'-L'A';
    if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||
       (base.size()==4&&(base.starts_with(L"COM")||base.starts_with(L"LPT"))&&
        ((base[3]>=L'1'&&base[3]<=L'9')||base[3]==L'\u00b9'||base[3]==L'\u00b2'||base[3]==L'\u00b3')))
      return fail(ErrorCode::InvalidInput,"reserved Windows device path");
    begin=end+1;
  }
  // Validate ordinary DOS spelling first; carry only this private extended form
  // to Win32 so reads, staging and handle-based rename share long-path semantics
  // without relying on the executable's longPathAware manifest.
  out.insert(0,L"\\\\?\\");
  return out;
}
bool regular(HANDLE h) {
  BY_HANDLE_FILE_INFORMATION info{};
  return GetFileInformationByHandle(h,&info) && GetFileType(h)==FILE_TYPE_DISK &&
    !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
Result<std::vector<std::byte>> read_file(const std::wstring& path,std::size_t limit) {
  if(!limit)return fail(ErrorCode::InvalidInput,"zero read limit");
  Handle f(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  if(!f.valid())return fail(GetLastError()==ERROR_FILE_NOT_FOUND?ErrorCode::NotFound:ErrorCode::IoFailure,"file open failed");
  if(!regular(f.value))return fail(ErrorCode::InvalidInput,"regular non-reparse file required");
  LARGE_INTEGER size{};
  if(!GetFileSizeEx(f.value,&size)||size.QuadPart<0)return fail(ErrorCode::IoFailure,"file size failed");
  if(static_cast<unsigned long long>(size.QuadPart)>limit)return fail(ErrorCode::Exhausted,"file exceeds read limit");
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t done=0;
  while(done<bytes.size()) {
    DWORD count=0;const DWORD want=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-done,1U<<20));
    if(!ReadFile(f.value,bytes.data()+done,want,&count,nullptr)||count==0)return fail(ErrorCode::IoFailure,"file read failed");
    done+=count;
  }
  return bytes;
}
std::atomic<std::uint64_t> next_stage{1};
Result<void> write_file(const std::wstring& path,std::span<const std::byte> bytes,bool immutable) {
  const auto attr=GetFileAttributesW(path.c_str());
  if(attr!=INVALID_FILE_ATTRIBUTES && (attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
    return fail(ErrorCode::InvalidInput,"replacement target must be regular");
  std::wstring stage;
  HANDLE raw=INVALID_HANDLE_VALUE;
  for(unsigned attempt=0;attempt<32&&raw==INVALID_HANDLE_VALUE;++attempt) {
    stage=path.substr(0,path.find_last_of(L'\\')+1)+L".race-stage-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(next_stage.fetch_add(1));
    raw=CreateFileW(stage.c_str(),GENERIC_WRITE|DELETE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(raw==INVALID_HANDLE_VALUE&&GetLastError()!=ERROR_FILE_EXISTS)return fail(ErrorCode::IoFailure,"staging creation failed");
  }
  if(raw==INVALID_HANDLE_VALUE)return fail(ErrorCode::Busy,"staging collisions");
  Handle f(raw);f.discard=true;
  std::size_t done=0;
  while(done<bytes.size()) {
    DWORD count=0;const DWORD want=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-done,1U<<20));
    if(!WriteFile(f.value,bytes.data()+done,want,&count,nullptr)||!count)return fail(ErrorCode::IoFailure,"staging write failed");
    done+=count;
  }
  if(!FlushFileBuffers(f.value))return fail(ErrorCode::IoFailure,"staging flush failed");
  // Publish the still-exclusive handle, never a close/reopen staging pathname.
  const auto info_bytes=sizeof(FILE_RENAME_INFO)+path.size()*sizeof(wchar_t);
  std::vector<std::max_align_t> storage((info_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
  auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
  rename->ReplaceIfExists=immutable?FALSE:TRUE;
  rename->RootDirectory=nullptr;
  rename->FileNameLength=static_cast<DWORD>(path.size()*sizeof(wchar_t));
  std::copy(path.begin(),path.end(),rename->FileName);
  if(SetFileInformationByHandle(f.value,FileRenameInfo,rename,static_cast<DWORD>(info_bytes))){f.discard=false;return {};}
  if(immutable&&(GetLastError()==ERROR_ALREADY_EXISTS||GetLastError()==ERROR_FILE_EXISTS)) {
    auto old=read_file(path,std::max<std::size_t>(1,bytes.size()));
    if(!old)return old.error();
    if(old.value().size()==bytes.size()&&std::equal(bytes.begin(),bytes.end(),old.value().begin()))return {};
    return fail(ErrorCode::Duplicate,"immutable name has different bytes");
  }
  return fail(ErrorCode::IoFailure,"atomic publication failed");
}
class WindowsClock final:public Clock {
  MonotonicTime now()const noexcept override {
    return {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()),2};
  }
};
class WindowsThread final:public Thread {
  std::thread thread_;
public:
  explicit WindowsThread(std::function<void()> body):thread_(std::move(body)){}
  ~WindowsThread()override{if(thread_.joinable()){if(thread_.get_id()==std::this_thread::get_id())std::abort();thread_.join();}}
  Result<void> join()noexcept override {
    if(!thread_.joinable())return {};
    if(thread_.get_id()==std::this_thread::get_id())return fail(ErrorCode::WrongThread,"cannot join own thread");
    try{thread_.join();return {};}catch(...){return fail(ErrorCode::PlatformFailure,"join failed");}
  }
};
class WindowsThreads final:public Threads {
  Result<std::unique_ptr<Thread>> start(std::function<void()> body)override{
    if(!body)return fail(ErrorCode::InvalidInput,"empty thread body");
    try{return std::unique_ptr<Thread>(new WindowsThread(std::move(body)));}
    catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"thread allocation");}
    catch(...){return fail(ErrorCode::PlatformFailure,"thread start failed");}
  }
};
class WindowsFiles final:public Files {
  Result<std::vector<std::byte>> read(std::string_view path,std::size_t limit)override {
    try{auto p=native_path(path);if(!p)return p.error();return read_file(p.value(),limit);}
    catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"file read allocation");}
    catch(const std::length_error&){return fail(ErrorCode::Exhausted,"file read capacity");}
  }
  Result<void> replace(std::string_view path,std::span<const std::byte> bytes,Durability d)override{
    if(d==Durability::Durable)return fail(ErrorCode::Unsupported,"power-loss durability not qualified");
    if(d!=Durability::AtomicVisibility)return fail(ErrorCode::InvalidInput,"invalid durability");
    try{auto p=native_path(path);if(!p)return p.error();return write_file(p.value(),bytes,false);}
    catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"replacement allocation");}
  }
};
class WindowsRoot final:public RootedFiles {
  std::wstring root_;
  // Directory handles deny delete sharing, preventing ancestor rename/removal.
  // The exclusive delete-on-close sentinel keeps even an empty root nonempty,
  // preventing a new reparse point. Write sharing admits child-file publication.
  // Unlike Linux's directory-fd capability, rename is Busy until release.
  std::vector<Handle> pins_;
  Result<std::wstring> leaf(std::string_view name){
    if(!valid_content_leaf(name))return fail(ErrorCode::InvalidInput,"invalid content leaf");
    return root_+L"\\"+std::wstring(name.begin(),name.end());
  }
public:
  WindowsRoot(std::wstring root,std::vector<Handle> pins):root_(std::move(root)),pins_(std::move(pins)){}
  Result<std::wstring> pinned_path() const {
    // The last pin is the sentinel; its predecessor is the actual directory.
    // Resolve its long normalized DOS name, including when admitted via 8.3.
    std::wstring path(32768,L'\0');
    const auto size=GetFinalPathNameByHandleW(pins_[pins_.size()-2].value,path.data(),static_cast<DWORD>(path.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size||size>=path.size())return fail(ErrorCode::IoFailure,"Cannot resolve pinned directory identity");
    path.resize(size);
    if(!path.starts_with(L"\\\\?\\")||path.size()<7||path[5]!=L':')return fail(ErrorCode::Unsupported,"Pinned directory lacks a local DOS identity");
    path.erase(0,4);return path;
  }
  Result<std::vector<std::byte>> read(std::string_view name,std::size_t limit)override {
    try{auto p=leaf(name);if(!p)return p.error();return read_file(p.value(),limit);}
    catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"rooted allocation");}
  }
  Result<void> write(std::string_view name,std::span<const std::byte> data,bool immutable){
    try{auto p=leaf(name);if(!p)return p.error();return write_file(p.value(),data,immutable);}
    catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"rooted allocation");}
  }
  Result<void> replace(std::string_view n,std::span<const std::byte> d,Durability p)override{
    if(p==Durability::Durable)return fail(ErrorCode::Unsupported,"power-loss durability not qualified");
    if(p!=Durability::AtomicVisibility)return fail(ErrorCode::InvalidInput,"invalid durability");
    return write(n,d,false);
  }
  Result<void> publish_immutable(std::string_view n,std::span<const std::byte> d)override{return write(n,d,true);}
};
}
Result<std::wstring> windows_pinned_root_path(const std::shared_ptr<RootedFiles>& root){
  const auto* native=dynamic_cast<const WindowsRoot*>(root.get());
  if(!native)return fail(ErrorCode::Unsupported,"Directory watch requires the Windows pinned root capability");
  try{return native->pinned_path();}
  catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Pinned directory identity allocation");}
}
bool valid_content_leaf(std::string_view p)noexcept {
  if(p.empty()||p.size()>96||p.front()=='.'||p.back()=='.'||p.find("..")!=p.npos)return false;
  for(char c:p)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='.'))return false;
  const auto base=p.substr(0,p.find('.'));
  if(base=="con"||base=="prn"||base=="aux"||base=="nul"||
     (base.size()==4&&(base.starts_with("com")||base.starts_with("lpt"))&&base[3]>='1'&&base[3]<='9'))return false;
  return true;
}
Result<std::shared_ptr<RootedFiles>> make_windows_rooted_files(std::string_view path){
  try{
    auto p=native_path(path);if(!p)return p.error();
    auto root=p.value();while(root.size()>7&&root.back()==L'\\')root.pop_back();
    wchar_t filesystem[32]{};
    if(!GetVolumeInformationW(root.substr(4,3).c_str(),nullptr,0,nullptr,nullptr,nullptr,filesystem,32)||
       std::wstring_view(filesystem)!=L"NTFS")
      return fail(ErrorCode::Unsupported,"root capability requires validated local NTFS");
    std::vector<Handle> pins;
    for(std::size_t end=7;;){
      end=root.find(L'\\',end);if(end==root.npos)end=root.size();
      auto prefix=root.substr(0,end);
      Handle pin(CreateFileW(prefix.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
      BY_HANDLE_FILE_INFORMATION info{};
      if(!pin.valid()||!GetFileInformationByHandle(pin.value,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return fail(ErrorCode::IoFailure,"cannot pin non-reparse content root");
      pins.push_back(std::move(pin));if(end==root.size())break;++end;
    }
    HANDLE sentinel=INVALID_HANDLE_VALUE;
    for(unsigned attempt=0;attempt<32&&sentinel==INVALID_HANDLE_VALUE;++attempt){
      auto name=L".race-root-pin-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(next_stage.fetch_add(1));
      UNICODE_STRING object_name{};
      object_name.Length=static_cast<USHORT>(name.size()*sizeof(wchar_t));
      object_name.MaximumLength=object_name.Length;
      object_name.Buffer=name.data();
      OBJECT_ATTRIBUTES attributes{};
      attributes.Length=sizeof(attributes);
      attributes.RootDirectory=pins.back().value;
      attributes.ObjectName=&object_name;
      attributes.Attributes=OBJ_CASE_INSENSITIVE|OBJ_DONT_REPARSE;
      IO_STATUS_BLOCK io{};
      HANDLE created=INVALID_HANDLE_VALUE;
      // A relative create never reopens the root pathname. Reject any reparse
      // encountered during admission, before even a temporary file can escape.
      const auto status=NtCreateFile(&created,FILE_GENERIC_READ|DELETE,&attributes,&io,nullptr,
          FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_TEMPORARY,0,FILE_CREATE,
          FILE_NON_DIRECTORY_FILE|FILE_DELETE_ON_CLOSE|FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_REPARSE_POINT,
          nullptr,0);
      if(status>=0)sentinel=created;
      else if(RtlNtStatusToDosError(status)!=ERROR_FILE_EXISTS&&RtlNtStatusToDosError(status)!=ERROR_ALREADY_EXISTS)
        return fail(ErrorCode::IoFailure,"cannot pin writable content directory");
    }
    if(sentinel==INVALID_HANDLE_VALUE)return fail(ErrorCode::Busy,"root sentinel collisions");
    Handle sentinel_owner(sentinel);
    // Seal the creation race: every retained ancestor must still be non-reparse.
    for(const auto& pin:pins){
      BY_HANDLE_FILE_INFORMATION info{};
      if(!GetFileInformationByHandle(pin.value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(ErrorCode::IoFailure,"content root changed during admission");
    }
    pins.push_back(std::move(sentinel_owner));
    return std::shared_ptr<RootedFiles>(new WindowsRoot(std::move(root),std::move(pins)));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"root capability allocation");}
}
Result<Services> make_windows_services(){
  try{return Services{std::make_unique<WindowsClock>(),std::make_unique<WindowsThreads>(),std::make_unique<WindowsFiles>()};}
  catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"platform allocation");}
}
Result<MemoryInfo> windows_memory_info(){
  MEMORYSTATUSEX mem{};mem.dwLength=sizeof(mem);PROCESS_MEMORY_COUNTERS process{};
  if(!GlobalMemoryStatusEx(&mem)||!GetProcessMemoryInfo(GetCurrentProcess(),&process,sizeof(process)))return fail(ErrorCode::PlatformFailure,"memory query failed");
  return MemoryInfo{mem.ullTotalPhys,process.WorkingSetSize,process.PeakWorkingSetSize};
}
void emergency_write(std::string_view message)noexcept{
  HANDLE h=GetStdHandle(STD_ERROR_HANDLE);
  while(!message.empty()){DWORD n=0;const auto want=static_cast<DWORD>(std::min<std::size_t>(message.size(),4096));if(!WriteFile(h,message.data(),want,&n,nullptr)||!n)break;message.remove_prefix(n);}
}
}
