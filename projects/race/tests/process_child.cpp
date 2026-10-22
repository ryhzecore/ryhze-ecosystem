#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
bool write(HANDLE handle,const void* bytes,std::size_t count){
  DWORD written{};return WriteFile(handle,bytes,static_cast<DWORD>(count),&written,nullptr)&&written==count;
}
bool text(std::string_view value){return write(GetStdHandle(STD_OUTPUT_HANDLE),value.data(),value.size());}
std::string utf8(std::wstring_view value){
  if(value.empty())return {};
  int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  if(!size)ExitProcess(2);std::string out(static_cast<std::size_t>(size),'\0');
  if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),size,nullptr,nullptr))ExitProcess(2);
  return out;
}
bool number(std::uint32_t value){
  std::array<unsigned char,4> bytes{};for(unsigned i=0;i<4;++i)bytes[i]=static_cast<unsigned char>(value>>(8*i));
  return write(GetStdHandle(STD_OUTPUT_HANDLE),bytes.data(),bytes.size());
}
}
int wmain(int argc,wchar_t** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(argc<2)return 2;const std::wstring_view mode=argv[1];
  if(mode==L"argv"){
    if(!number(static_cast<std::uint32_t>(argc-2)))return 3;
    for(int i=2;i<argc;++i){auto value=utf8(argv[i]);if(!number(static_cast<std::uint32_t>(value.size()))||!text(value))return 3;}
    return 0;
  }
  if(mode==L"exit"&&argc==3)ExitProcess(static_cast<UINT>(std::stoul(argv[2])));
  if(mode==L"abrupt"){
    constexpr char message[]="before abrupt termination\n";
    if(!write(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1))return 3;
    TerminateProcess(GetCurrentProcess(),0xc0000005U);return 4;
  }
  if(mode==L"flood"&&argc==4){
    auto out=std::stoul(argv[2]),err=std::stoul(argv[3]);if(out>1048576||err>1048576)return 2;
    std::array<char,512> o{},e{};o.fill('o');e.fill('e');
    while(out||err){
      const auto a=std::min<std::size_t>(out,o.size()),b=std::min<std::size_t>(err,e.size());
      if(a&&!write(GetStdHandle(STD_OUTPUT_HANDLE),o.data(),a))return 3;
      if(b&&!write(GetStdHandle(STD_ERROR_HANDLE),e.data(),b))return 3;
      out-=static_cast<unsigned long>(a);err-=static_cast<unsigned long>(b);
    }
    return 0;
  }
  if(mode==L"gate"&&argc==3){
    const auto gate=OpenEventW(SYNCHRONIZE,FALSE,argv[2]);if(!gate)return 2;
    if(!text("READY "+std::to_string(GetCurrentProcessId())+"\n")){CloseHandle(gate);return 3;}
    const auto result=WaitForSingleObject(gate,INFINITE);CloseHandle(gate);return result==WAIT_OBJECT_0?0:3;
  }
  if(mode==L"handle"&&argc==3){
    const auto handle=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(argv[2])));
    const bool inherited=SetEvent(handle)!=FALSE;text(inherited?"INHERITED":"BLOCKED");return inherited?1:0;
  }
  if(mode==L"deny_child"){
    std::vector<wchar_t> path(32768);const auto size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!size||size>=path.size())return 2;
    std::wstring command=L"\""+std::wstring(path.data(),size)+L"\" exit 0";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(CreateProcessW(path.data(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)){
      TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,INFINITE);
      CloseHandle(process.hThread);CloseHandle(process.hProcess);text("ALLOWED");return 1;
    }
    text("DENIED");return 0;
  }
  if(mode==L"memory"){
    void* memory=VirtualAlloc(nullptr,300U*1024U*1024U,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(memory){VirtualFree(memory,0,MEM_RELEASE);text("ALLOWED");return 1;}text("DENIED");return 0;
  }
  return 2;
}
