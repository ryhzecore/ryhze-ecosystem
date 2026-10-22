#include "editor_reimport_worker.hpp"
#include "test.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
using namespace race;
namespace fs=std::filesystem;
namespace d=desktop;
std::string utf8(std::wstring_view value) {
  CHECK(value.size()<=32767);
  const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  CHECK(size>0);std::string text(static_cast<std::size_t>(size),'\0');
  CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),size,nullptr,nullptr)==size);
  return text;
}
fs::path path(std::string_view value) {
  CHECK(value.size()<=32767);
  const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
  CHECK(size>0);std::wstring text(static_cast<std::size_t>(size),L'\0');
  CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),size)==size);
  return fs::path(text);
}
std::size_t children(const fs::path& root) {
  std::size_t count{};for(const auto& entry:fs::directory_iterator(root))if(entry.is_directory())++count;return count;
}
std::string read(const fs::path& file) {
  std::ifstream input(file,std::ios::binary);CHECK(input);return {std::istreambuf_iterator<char>(input),{}};
}
void run(const fs::path& root,const std::string& executable) {
  const auto source=root/L"source",products=root/L"products",checkpoints=root/L"checkpoints";
  CHECK(fs::create_directory(source));CHECK(fs::create_directory(products));CHECK(fs::create_directory(checkpoints));
  const d::MeshReimportRequest request{utf8(source.native()),utf8(products.native()),utf8(checkpoints.native()),utf8(checkpoints.native()),1};
  auto factory=d::editor_reimport_workers(executable);fs::path first_path,second_path;
  constexpr std::string_view receipt="preserved native stage evidence";
  const auto bytes=std::as_bytes(std::span(receipt));
  {
    auto first=factory(1,request),second=factory(2,request);CHECK(first&&second);
    CHECK(first.value().stage&&first.value().process&&second.value().stage&&second.value().process);
    CHECK(first.value().executable==executable&&second.value().executable==executable);
    first_path=path(first.value().directory);second_path=path(second.value().directory);
    CHECK(first_path.is_absolute()&&second_path.is_absolute()&&!fs::equivalent(first_path,second_path));
    CHECK(fs::equivalent(first_path.parent_path(),checkpoints)&&fs::equivalent(second_path.parent_path(),checkpoints));
    CHECK_ERROR(first.value().stage->read("input.obj",1),ErrorCode::NotFound);
    CHECK_ERROR(first.value().stage->read("result.rasset",1),ErrorCode::NotFound);
    CHECK(first.value().stage->publish_immutable("receipt.txt",bytes));
    CHECK(second.value().stage->publish_immutable("receipt.txt",bytes));
    CHECK(read(first_path/L"receipt.txt")==receipt&&read(second_path/L"receipt.txt")==receipt);
    // Even without old worker output, a previous stage identity is not reusable.
    CHECK_ERROR(factory(1,request),ErrorCode::Duplicate);CHECK(children(checkpoints)==2);
    // Pinned stage/parent identity survives until its process owner releases it.
    std::error_code rename_error;fs::rename(first_path,checkpoints/L"unexpected-move",rename_error);CHECK(rename_error);
  }
  CHECK(fs::is_directory(first_path)&&fs::is_directory(second_path));
  CHECK(read(first_path/L"receipt.txt")==receipt&&read(second_path/L"receipt.txt")==receipt);
  {
    auto aliased=request;auto upper=source.native();
    for(auto& c:upper)if(c>=L'a'&&c<=L'z')c=static_cast<wchar_t>(c-L'a'+L'A');
    aliased.product_root=utf8(upper);CHECK_ERROR(factory(3,aliased),ErrorCode::InvalidInput);
    CHECK(children(checkpoints)==2);
  }
  {
    const auto nested=source/L"checkpoints";CHECK(fs::create_directory(nested));
    auto invalid=request;invalid.checkpoint_root=utf8(nested.native());
    CHECK_ERROR(factory(4,invalid),ErrorCode::InvalidInput);CHECK(children(nested)==0&&children(checkpoints)==2);
  }
  {
    const auto nested=products/L"staging";CHECK(fs::create_directory(nested));
    auto invalid=request;invalid.staging_root=utf8(nested.native());
    CHECK_ERROR(factory(5,invalid),ErrorCode::InvalidInput);CHECK(children(nested)==0&&children(checkpoints)==2);
  }
  {
    auto invalid=request;invalid.staging_root=request.source_root;
    CHECK_ERROR(factory(6,invalid),ErrorCode::InvalidInput);CHECK(children(source)==1&&children(checkpoints)==2);
  }
  {
    // Factory preparation does not launch or probe the fixed executable. The
    // native Service diagnoses a missing sibling at start; stage evidence stays.
    const auto missing=root/L"missing-worker.exe";CHECK(!fs::exists(missing));
    auto absent=d::editor_reimport_workers(utf8(missing.native()));auto prepared=absent(7,request);CHECK(prepared);
    const auto preserved=path(prepared.value().directory);
    CHECK_ERROR(prepared.value().process->start({prepared.value().executable,prepared.value().directory,{}}),ErrorCode::NotFound);
    CHECK(prepared.value().stage->publish_immutable("receipt.txt",bytes));
    prepared.value().process.reset();prepared.value().stage.reset();
    CHECK(read(preserved/L"receipt.txt")==receipt&&children(checkpoints)==3);
  }
  CHECK_ERROR(factory(0,request),ErrorCode::InvalidInput);
  std::printf("native factory checks passed; fresh stages, aliases and failure evidence retained\n");
}
}
int wmain(int argc,wchar_t** argv) {
  CHECK(argc==1||argc==2);
  const auto root=argc==2?fs::absolute(argv[1]):fs::temp_directory_path()/
      (L"race-native-reimport-"+std::to_wstring(GetCurrentProcessId())+L"-"+
       std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
  // Optional argument must be a fresh root with an existing parent. No fixture
  // cleanup occurs: repeated no-argument CTest runs preserve distinct evidence.
  CHECK(fs::create_directory(root));const auto name=utf8(root.native());
  std::printf("native reimport factory evidence: %s\n",name.c_str());std::fflush(stdout);
  std::wstring image(32768,L'\0');const auto count=GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));
  CHECK(count&&count<image.size());image.resize(count);
  run(root,utf8(image));return 0;
}
