#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mesh_pipeline.hpp"
#include "race/build_identity.hpp"
#include <cstdio>
#include <string>

namespace {
using namespace race;
Result<std::string> utf8(std::wstring_view value){
  if(value.size()>16384)return fail(ErrorCode::Exhausted,"Worker argument limit");
  const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  if(size<=0)return fail(ErrorCode::InvalidInput,"Worker argument encoding");
  std::string out(static_cast<std::size_t>(size),'\0');
  if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),size,nullptr,nullptr)!=size)
    return fail(ErrorCode::PlatformFailure,"Worker argument conversion");
  return out;
}
int error(Error reason){std::fprintf(stderr,"mesh_worker_error code=%u reason=%s\n",unsigned(reason.code),reason.message);return 1;}
}
int wmain(int argc,wchar_t** argv){
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
  if(argc==2&&std::wstring_view(argv[1])==L"--identity"){
    std::printf("%s %s\n",race::build::source_commit,race::build::authored_input_sha256);return 0;
  }
  if(argc!=3&&argc!=4)return 2;
  const auto profile=argc==3?mesh_pipeline::Profile::Geometry:mesh_pipeline::Profile::GeometryUv;
  if(argc==4&&std::wstring_view(argv[3])!=L"geometry-uv")return 2;
  try{
    auto root=utf8(argv[1]),identity=utf8(argv[2]);if(!root)return error(root.error());if(!identity)return error(identity.error());
    auto id=assets::parse_id(identity.value());if(!id)return error(id.error());
    auto files=platform::make_rooted_files(root.value());if(!files)return error(files.error());
    auto input=files.value()->read("input.obj",mesh_pipeline::Limits{}.source_bytes);if(!input)return error(input.error());
    auto product=mesh_pipeline::import_obj(id.value(),input.value(),profile);if(!product)return error(product.error());
    auto written=files.value()->publish_immutable("result.rasset",product.value());if(!written)return error(written.error());
    std::printf("mesh_worker_result bytes=%zu\n",product.value().size());return 0;
  }catch(const std::bad_alloc&){return error(fail(ErrorCode::Exhausted,"Worker allocation"));}
}
