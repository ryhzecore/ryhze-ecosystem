#include "race/assets/assets.hpp"
#include "race/build_identity.hpp"
#include <cstdio>
#include <fstream>
#include <string>
using namespace race;
using namespace race::assets;
namespace {
int error(Error e) {
  std::fprintf(stderr, "asset_error code=%u reason=%s\n", unsigned(e.code), e.message);
  return 1;
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--identity") {
    std::printf("%s %s\n", build::source_commit, build::authored_input_sha256);
    return 0;
  }
  if (argc < 3) {
    std::fprintf(
        stderr,
        "usage: race_assets_tool cook|inspect|validate ABSOLUTE_ROOT [readable inputs...]\n");
    return 2;
  }
  try {
    auto files = platform::make_rooted_files(argv[2]);
    if (!files)
      return error(files.error());
    std::string_view mode = argv[1];
    Catalog catalog;
    if (mode == "cook") {
      if (argc < 4 || argc > 131)
        return 2;
      std::vector<Input> inputs;
      for (int i = 3; i < argc; ++i) {
        std::ifstream stream(argv[i], std::ios::binary);
        if (!stream)
          return error(fail(ErrorCode::IoFailure, "fixture input open failed"));
        std::array<char, 4097> buffer{};
        stream.read(buffer.data(), buffer.size());
        auto n = stream.gcount();
        if (n > 4096)
          return error(fail(ErrorCode::Exhausted, "fixture input limit"));
        auto input = parse_input(std::string_view(buffer.data(), n));
        if (!input)
          return error(input.error());
        inputs.push_back(std::move(input.value()));
      }
      Diagnostic d;
      auto result = cook(inputs, *files.value(), {}, &d);
      if (!result) {
        std::fprintf(stderr, "asset=%s cause=%s stage=%u\n", id_text(d.asset).c_str(),
                     id_text(d.cause).c_str(), unsigned(d.stage));
        return error(result.error());
      }
      catalog = std::move(result.value());
    } else if (mode == "inspect" || mode == "validate") {
      auto bytes = files.value()->read("catalog.bin", Limits{}.catalog_bytes);
      if (!bytes)
        return error(bytes.error());
      Diagnostic d;
      auto parsed = decode_catalog(bytes.value(), {}, &d);
      if (!parsed)
        return error(parsed.error());
      catalog = std::move(parsed.value());
    } else
      return 2;
    std::printf("{\"format\":\"RACECAT1\",\"version\":1,\"source\":\"%s\",\"authored_sha256\":\"%"
                "s\",\"entries\":[",
                build::source_commit, build::authored_input_sha256);
    bool first = true;
    for (const auto &e : catalog.entries) {
      auto bytes = files.value()->read(e.location, Limits{}.product_bytes);
      if (!bytes)
        return error(bytes.error());
      auto product = validate_product(bytes.value(), e);
      if (!product)
        return error(product.error());
      std::printf("%s{\"id\":\"%s\",\"revision\":\"%s\",\"type\":%u,\"target\":%u,\"schema\":%u,"
                  "\"bytes\":%llu,\"values\":%llu,\"hard\":[",
                  first ? "" : ",", id_text(e.id).c_str(), hex(e.revision).c_str(), e.type,
                  e.target, e.schema, (unsigned long long)e.bytes,
                  (unsigned long long)product.value().count);
      first = false;
      for (std::size_t i = 0; i < e.hard.size(); ++i)
        std::printf("%s{\"id\":\"%s\",\"revision\":\"%s\"}", i ? "," : "",
                    id_text(e.hard[i].id).c_str(), hex(e.hard[i].revision).c_str());
      std::printf("],\"soft\":[");
      for (std::size_t i = 0; i < e.soft.size(); ++i)
        std::printf("%s\"%s\"", i ? "," : "", id_text(e.soft[i]).c_str());
      std::printf("]}");
    }
    std::printf("]}\n");
    return 0;
  } catch (const std::bad_alloc &) {
    return error(fail(ErrorCode::Exhausted, "asset tool allocation"));
  }
}
