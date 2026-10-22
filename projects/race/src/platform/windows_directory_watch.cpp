#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "directory_invalidation.hpp"
#include "windows_root_access.hpp"
#include "windows_watch_calls.hpp"
#include <array>
#include <thread>

namespace race::platform::directory_watch {
namespace {
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
  Handle()=default;Handle(const Handle&)=delete;
};
class WindowsWatch final:public Service {
  std::shared_ptr<RootedFiles> root_;
  detail::Invalidation state_;
  detail::Calls calls_;
  std::thread::id owner_{std::this_thread::get_id()};
  Handle directory_,event_;
  OVERLAPPED request_{};
  std::array<DWORD,4096> buffer_{}; // DWORD-aligned 16 KiB, fixed for handle lifetime.
  bool pending_{};
  bool owner() const noexcept {return owner_==std::this_thread::get_id();}
  Result<void> arm() noexcept {
    request_={};request_.hEvent=event_.value;
    if(!calls_.reset_event(event_.value))return fail(ErrorCode::PlatformFailure,"Directory watch event reset failed");
    constexpr DWORD filter=FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|
        FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_ATTRIBUTES;
    if(!calls_.arm(directory_.value,buffer_.data(),static_cast<DWORD>(sizeof(buffer_)),FALSE,filter,nullptr,&request_,nullptr))
      return fail(GetLastError()==ERROR_NOTIFY_ENUM_DIR?ErrorCode::Overflow:ErrorCode::PlatformFailure,"Cannot arm directory invalidation");
    pending_=true;return {};
  }
  Result<void> stop() noexcept {
    if(state_.snapshot().phase==Phase::Stopped)return {};
    state_.stop_requested();
    if(pending_&&!calls_.cancel(directory_.value,&request_)&&GetLastError()!=ERROR_NOT_FOUND){
      auto error=fail(ErrorCode::PlatformFailure,"Directory cancellation request failed");state_.failure(error);return error;
    }
    if(!pending_)state_.stopped();return {};
  }
public:
  WindowsWatch(std::shared_ptr<RootedFiles> root,std::uint64_t identity,Limits limits,detail::Calls calls)
      :root_(std::move(root)),state_(identity,limits.generations),calls_(calls){}
  ~WindowsWatch() override {
    (void)stop();
    if(pending_){
      DWORD count{};
      // Cancellation is only a request. Even a failed cancellation retains the
      // buffer/OVERLAPPED until GetOverlappedResult observes physical completion.
      for(;;){
        if(GetOverlappedResult(directory_.value,&request_,&count,TRUE))break;
        const auto error=GetLastError();if(error!=ERROR_IO_INCOMPLETE&&error!=ERROR_IO_PENDING)break;
      }
      pending_=false;
    }
  }
  Result<void> open(std::wstring_view path){
    const std::wstring owned=L"\\\\?\\"+std::wstring(path);
    directory_.value=CreateFileW(owned.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
      OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OVERLAPPED,nullptr);
    if(directory_.value==INVALID_HANDLE_VALUE)return fail(ErrorCode::IoFailure,"Cannot open pinned directory watch");
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(directory_.value,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
      return fail(ErrorCode::InvalidInput,"Directory watch requires a non-reparse root");
    event_.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!event_.value)return fail(ErrorCode::PlatformFailure,"Directory watch event allocation");
    return arm();
  }
  Result<Snapshot> poll() noexcept override {
    if(!owner())return fail(ErrorCode::WrongThread,"Directory watch poll requires owner");
    if(!pending_){
      if(state_.snapshot().phase==Phase::Watching){auto started=arm();if(!started)state_.arm_failed(started.error());}
      return state_.snapshot();
    }
    DWORD bytes{};const bool success=GetOverlappedResult(directory_.value,&request_,&bytes,FALSE)!=FALSE;
    const auto error=success?ERROR_SUCCESS:GetLastError();
    if(error==ERROR_IO_INCOMPLETE||error==ERROR_IO_PENDING)return state_.snapshot();
    pending_=false;
    if(success||error==ERROR_NOTIFY_ENUM_DIR){
      (void)state_.batch(!bytes||error==ERROR_NOTIFY_ENUM_DIR);
    }else if(error!=ERROR_OPERATION_ABORTED||state_.snapshot().phase==Phase::Watching){
      state_.failure(fail(ErrorCode::IoFailure,"Directory notification failed"));
    }
    if(state_.snapshot().phase==Phase::Watching&&!state_.snapshot().failure){
      auto started=arm();if(!started)state_.arm_failed(started.error());
    }else state_.stopped();
    return state_.snapshot();
  }
  Result<void> acknowledge(Observation observation) noexcept override {
    auto current=poll();if(!current)return current.error();return state_.acknowledge(observation);
  }
  Result<void> request_stop() noexcept override {
    if(!owner())return fail(ErrorCode::WrongThread,"Directory watch stop requires owner");return stop();
  }
  Result<Snapshot> wait_stopped(std::uint32_t milliseconds) noexcept override {
    if(!owner())return fail(ErrorCode::WrongThread,"Directory watch wait requires owner");
    if(milliseconds>60000)return fail(ErrorCode::InvalidInput,"Directory stop wait limit");
    if(state_.snapshot().phase==Phase::Watching)return fail(ErrorCode::Busy,"Directory watch stop was not requested");
    const auto start=GetTickCount64();
    for(;;){
      auto current=poll();if(!current)return current.error();if(current.value().phase==Phase::Stopped)return current;
      const auto elapsed=GetTickCount64()-start;if(elapsed>=milliseconds)return fail(ErrorCode::Timeout,"Directory stop completion timeout");
      const auto wait=WaitForSingleObject(event_.value,static_cast<DWORD>(milliseconds-elapsed));
      if(wait==WAIT_FAILED)return fail(ErrorCode::PlatformFailure,"Directory completion wait failed");
    }
  }
};
}
Result<std::unique_ptr<Service>> detail::make_with_calls(std::shared_ptr<RootedFiles> root,std::uint64_t identity,const Limits& limits,Calls calls){
  if(!root||!identity||!limits.generations||!calls.reset_event||!calls.arm||!calls.cancel)
    return fail(ErrorCode::InvalidInput,"Invalid directory watch admission");
  try{
    auto path=windows_pinned_root_path(root);if(!path)return path.error();
    auto watch=std::make_unique<WindowsWatch>(std::move(root),identity,limits,calls);
    auto opened=watch->open(path.value());if(!opened)return opened.error();return std::unique_ptr<Service>(std::move(watch));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Directory watch allocation");}
}
Result<std::unique_ptr<Service>> make_windows_service(std::shared_ptr<RootedFiles> root,std::uint64_t identity,const Limits& limits){
  return detail::make_with_calls(std::move(root),identity,limits,{});
}
} // namespace race::platform::directory_watch
