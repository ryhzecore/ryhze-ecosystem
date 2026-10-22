#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "mesh_pipeline.hpp"
#include "race/build_identity.hpp"
#include <array>
#include <cstdio>

namespace {
using namespace race;
namespace m = assets::mesh;
int error(Error e) {
  std::fprintf(stderr, "mesh_error code=%u reason=%s\n", unsigned(e.code), e.message);
  return 1;
}
int report(std::span<const std::byte> bytes, std::string_view leaf, bool leased) {
  auto decoded = m::validate(bytes); if (!decoded) return error(decoded.error());
  const auto &v = decoded.value();
  std::printf("{\"format\":\"RACEAST1\",\"source_commit\":\"%s\",\"authored_sha256\":\"%s\","
              "\"id\":\"%s\",\"revision\":\"%s\",\"leaf\":\"%.*s\",\"representation\":%u,"
              "\"target\":%u,\"vertices\":%zu,\"indices\":%zu,\"leased\":%s}\n",
              build::source_commit, build::authored_input_sha256,
              assets::id_text(v.id()).c_str(), assets::hex(v.revision()).c_str(),
              static_cast<int>(leaf.size()), leaf.data(), unsigned(v.stamp().representation),
              v.stamp().target, v.vertex_count(), v.index_count(), leased ? "true" : "false");
  return 0;
}
} // namespace
int run(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--identity") {
    std::printf("%s %s\n", build::source_commit, build::authored_input_sha256); return 0;
  }
  if (argc < 4) {
    std::fprintf(stderr, "usage: race_mesh_tool import SOURCE_ROOT source.obj OUTPUT_ROOT ID\n"
                        "       race_mesh_tool cook INPUT_ROOT imported.rasset OUTPUT_ROOT\n"
                        "       race_mesh_tool inspect|validate|load ROOT product.rasset\n");
    return 2;
  }
  try {
    const std::string_view mode(argv[1]), leaf(argv[3]);
    const bool importing = mode == "import", cooking = mode == "cook", loading = mode == "load";
    if ((importing && argc != 6) || (cooking && argc != 5) ||
        (!importing && !cooking && (argc != 4 || (mode != "inspect" && mode != "validate" && !loading))))
      return 2;
    if (!platform::valid_content_leaf(leaf) || !leaf.ends_with(importing ? ".obj" : ".rasset"))
      return error(fail(ErrorCode::InvalidInput, "mesh tool requires a canonical source or native leaf"));
    auto input = platform::make_rooted_files(argv[2]); if (!input) return error(input.error());
    auto bytes = input.value()->read(leaf, importing ? mesh_pipeline::Limits{}.source_bytes : m::Limits{}.product_bytes);
    if (!bytes) return error(bytes.error());
    if (importing || cooking) {
      Result<std::vector<std::byte>> product = fail(ErrorCode::Invariant, "uninitialized mesh tool product");
      if (importing) {
        auto id = assets::parse_id(argv[5]); if (!id) return error(id.error());
        product = mesh_pipeline::import_obj(id.value(), bytes.value());
      } else product = mesh_pipeline::cook(bytes.value());
      if (!product) return error(product.error());
      auto output = platform::make_rooted_files(argv[4]); if (!output) return error(output.error());
      auto published = mesh_pipeline::publish(*output.value(), product.value());
      if (!published) return error(published.error());
      return report(product.value(), published.value(), false);
    }
    if (loading) {
      auto decoded = m::validate(bytes.value()); if (!decoded) return error(decoded.error());
      if (decoded.value().stamp().representation != m::Representation::Cooked)
        return error(fail(ErrorCode::InvalidInput, "mesh load requires cooked native input"));
      auto allocator = TrackedAllocator::create({64 * 1024 * 1024});
      if (!allocator) return error(allocator.error());
      auto store = m::Store::create({}, allocator.value()); if (!store) return error(store.error());
      auto update = store.value()->begin_update(decoded.value().id(), decoded.value().stamp());
      if (!update) return error(update.error());
      auto published = store.value()->publish(update.value(), bytes.value()); if (!published) return error(published.error());
      auto lease = store.value()->acquire(published.value()); if (!lease) return error(lease.error());
      bytes.value().clear(); // Report through the owning lease, after caller input is gone.
      return report(lease.value()->view().bytes(), leaf, true);
    }
    return report(bytes.value(), leaf, false);
  } catch (const std::bad_alloc &) { return error(fail(ErrorCode::Exhausted, "mesh tool allocation")); }
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
  if(argc<2||argc>6)return 2;
  try{
    std::array<std::string,6> arguments;
    std::array<char*,6> pointers{};
    for(int n=0;n<argc;++n){
      const std::wstring_view value(argv[n]);
      if(value.empty()||value.size()>16384)return error(race::fail(race::ErrorCode::InvalidInput,"Mesh argument length"));
      const int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
      if(length<=0)return error(race::fail(race::ErrorCode::InvalidInput,"Mesh argument encoding"));
      auto& text=arguments[static_cast<std::size_t>(n)];text.resize(static_cast<std::size_t>(length));
      if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),text.data(),length,nullptr,nullptr)!=length)
        return error(race::fail(race::ErrorCode::PlatformFailure,"Mesh argument conversion"));
      pointers[static_cast<std::size_t>(n)]=text.data();
    }
    return run(argc,pointers.data());
  }catch(const std::bad_alloc&){return error(race::fail(race::ErrorCode::Exhausted,"Mesh arguments allocation"));}
}
#else
int main(int argc,char** argv){return run(argc,argv);}
#endif
