#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "editor_reimport_worker.hpp"
#include "platform/windows_root_access.hpp"
#include <filesystem>

namespace race::desktop {
namespace {
namespace fs=std::filesystem;
constexpr std::size_t maximum_path_bytes=4096;
bool bounded_path(const std::string& value) noexcept {
  return !value.empty()&&value.size()<=maximum_path_bytes&&
      value.capacity()<=maximum_path_bytes&&value.find('\0')==std::string::npos;
}
bool same_path(std::wstring_view a,std::wstring_view b) noexcept {
  return a.size()==b.size()&&CompareStringOrdinal(a.data(),static_cast<int>(a.size()),
      b.data(),static_cast<int>(b.size()),TRUE)==CSTR_EQUAL;
}
bool inside(std::wstring_view root,std::wstring_view path) noexcept {
  while(root.size()>3&&root.back()==L'\\')root.remove_suffix(1);
  if(path.size()<root.size()||!same_path(root,path.substr(0,root.size())))return false;
  return path.size()==root.size()||root.back()==L'\\'||path[root.size()]==L'\\';
}
Result<std::string> utf8(std::wstring_view value) {
  if(value.empty()||value.size()>32767)return fail(ErrorCode::InvalidInput,"Reimport stage path length");
  const auto count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
  if(count<=0)return fail(ErrorCode::InvalidInput,"Reimport stage path encoding");
  if(static_cast<std::size_t>(count)>maximum_path_bytes)return fail(ErrorCode::Exhausted,"Reimport stage path byte limit");
  std::string result(static_cast<std::size_t>(count),'\0');
  if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr)!=count)
    return fail(ErrorCode::PlatformFailure,"Reimport stage path conversion");
  if(result.capacity()>maximum_path_bytes)return fail(ErrorCode::Exhausted,"Reimport stage path capacity limit");
  return result;
}
}
MeshReimportTask::WorkerFactory editor_reimport_workers(std::string executable) {
  return [executable=std::move(executable)](std::uint64_t identity,const MeshReimportRequest& names)->Result<MeshReimportWorker> {
    if(!identity||!bounded_path(executable)||!bounded_path(names.source_root)||
        !bounded_path(names.product_root)||!bounded_path(names.checkpoint_root)||!bounded_path(names.staging_root))
      return fail(ErrorCode::InvalidInput,"Invalid native reimport worker paths or identity");
    try {
      // Resolve capabilities, not caller spelling: this also detects case and
      // short-name aliases. Parent pins prevent rename/reparse while creating.
      auto source=platform::make_rooted_files(names.source_root);if(!source)return source.error();
      auto products=platform::make_rooted_files(names.product_root);if(!products)return products.error();
      auto checkpoints=platform::make_rooted_files(names.checkpoint_root);if(!checkpoints)return checkpoints.error();
      auto parent=platform::make_rooted_files(names.staging_root);if(!parent)return parent.error();
      auto source_path=platform::windows_pinned_root_path(source.value());if(!source_path)return source_path.error();
      auto product_path=platform::windows_pinned_root_path(products.value());if(!product_path)return product_path.error();
      auto checkpoint_path=platform::windows_pinned_root_path(checkpoints.value());if(!checkpoint_path)return checkpoint_path.error();
      auto parent_path=platform::windows_pinned_root_path(parent.value());if(!parent_path)return parent_path.error();
      const auto stage_path=fs::path(parent_path.value())/(L"race-import-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(identity));
      const auto& candidate=stage_path.native();
      if(inside(source_path.value(),product_path.value())||inside(source_path.value(),checkpoint_path.value())||
          inside(source_path.value(),candidate)||inside(product_path.value(),candidate))
        return fail(ErrorCode::InvalidInput,"Reimport outputs must be outside source and staging outside products");
      auto directory=utf8(candidate);if(!directory)return directory.error();
      std::error_code ec;
      if(!fs::create_directory(stage_path,ec))return fail(ec?ErrorCode::IoFailure:ErrorCode::Duplicate,"Reimport stage must be a fresh directory");
      // Do not remove anything after exclusive creation, including on failure.
      // The stage capability pins every ancestor and survives child quiescence.
      auto stage=platform::make_rooted_files(directory.value());if(!stage)return stage.error();
      auto actual=platform::windows_pinned_root_path(stage.value());if(!actual)return actual.error();
      if(!same_path(actual.value(),candidate)||inside(source_path.value(),actual.value())||inside(product_path.value(),actual.value()))
        return fail(ErrorCode::InvalidInput,"Reimport stage actual root differs from the admitted directory");
      auto process=platform::process::make_windows_service(identity);if(!process)return process.error();
      return MeshReimportWorker{std::move(stage).value(),executable,std::move(directory).value(),std::move(process).value()};
    }catch(const std::bad_alloc&) {return fail(ErrorCode::Exhausted,"Native reimport worker allocation");}
     catch(const fs::filesystem_error&) {return fail(ErrorCode::IoFailure,"Native reimport staging path failure");}
  };
}
} // namespace race::desktop
