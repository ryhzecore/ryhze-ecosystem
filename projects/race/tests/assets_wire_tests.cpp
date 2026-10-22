#include "assets_support.hpp"
#include <filesystem>
#include <fstream>
#include <random>
#ifndef _WIN32
#include <unistd.h>
#endif
namespace {
void set(std::vector<std::byte> &b, std::size_t at, std::uint64_t v, std::size_t n = 8) {
  for (std::size_t i = 0; i < n; ++i)
    b[at + i] = std::byte((v >> (i * 8)) & 255);
}
} // namespace
void asset_wire_tests() {
  CHECK(hex(digest({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string abc = "abc";
  CHECK(hex(digest(std::as_bytes(std::span(abc)))) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto files = std::make_shared<FakeFiles>();
  auto inputs = diamond();
  auto result = cook(inputs, *files);
  CHECK(result);
  auto c = result.value();
  auto encoded = encode_catalog(c);
  CHECK(encoded);
  CHECK(decode_catalog(encoded.value()));
  auto before = files->data;
  std::reverse(inputs.begin(), inputs.end());
  CHECK(cook(inputs, *files));
  CHECK(files->data == before);
  auto duplicate = inputs;
  duplicate.push_back(inputs[0]);
  CHECK_ERROR(cook(duplicate, *files), ErrorCode::Duplicate);
  duplicate[0].id = {};
  CHECK_ERROR(cook(duplicate, *files), ErrorCode::InvalidInput);
  auto missing = inputs;
  missing[0].hard = {aid(90)};
  Diagnostic d;
  CHECK_ERROR(cook(missing, *files, {}, &d), ErrorCode::NotFound);
  CHECK(d.cause == aid(90));
  auto wrong = c;
  wrong.entries[0].type = 99;
  CHECK_ERROR(validate_catalog(wrong, {}), ErrorCode::IncompatibleVersion);
  wrong = c;
  wrong.entries[0].target = 99;
  CHECK_ERROR(validate_catalog(wrong, {}), ErrorCode::IncompatibleVersion);
  wrong = c;
  wrong.entries[0].id = {};
  CHECK_ERROR(validate_catalog(wrong, {}), ErrorCode::InvalidInput);
  for (auto path : {"../file", "/absolute", "A.bin", "dir/file", "dir\\file", "a..b"}) {
    wrong = c;
    wrong.entries[0].location = path;
    CHECK(!validate_catalog(wrong, {}));
    CHECK(!platform::valid_content_leaf(path));
  }
  auto bytes = encoded.value();
  for (std::size_t n = 0; n < bytes.size(); ++n)
    CHECK(!decode_catalog(std::span(bytes).first(n)));
  for (auto at : {8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u, 48u, 56u, 64u + 144u, 64u + 148u,
                  64u + 152u, 64u + 160u}) {
    auto bad = bytes;
    set(bad, at, UINT64_MAX, at < 32 ? 4 : 8);
    CHECK(!decode_catalog(bad));
  }
  auto entry = c.entries[0];
  auto product = files->data[entry.location];
  CHECK(validate_product(product, entry));
  for (std::size_t n = 0; n < product.size(); ++n)
    CHECK(!validate_product(std::span(product).first(n), entry));
  for (auto at : {8u, 12u, 16u, 20u, 24u, 28u, 32u, 40u, 64u, 104u, 108u, 112u, 120u}) {
    {
      auto malformed = product;
      auto expected = entry;
      for (std::size_t i = 0; i < 16; ++i)
        malformed[128 + i] = std::byte{};
      expected.hard[0].id = {};
      expected.revision = digest(malformed);
      CHECK_ERROR(validate_product(malformed, expected), ErrorCode::InvalidDependency);
      malformed = product;
      expected = entry;
      std::swap_ranges(malformed.begin() + 128, malformed.begin() + 176, malformed.begin() + 176);
      std::swap(expected.hard[0], expected.hard[1]);
      expected.revision = digest(malformed);
      CHECK_ERROR(validate_product(malformed, expected), ErrorCode::InvalidDependency);
      auto cyclic = c;
      cyclic.entries[3].hard.push_back({cyclic.entries[0].id, cyclic.entries[0].revision});
      Diagnostic path;
      CHECK_ERROR(validate_catalog(cyclic, {}, &path), ErrorCode::Cycle);
      CHECK(path.path_size > 1);
    }
    auto bad = product;
    set(bad, at, UINT64_MAX, (at < 32 || at == 104 || at == 108) ? 4 : 8);
    auto e = entry;
    e.revision = digest(bad);
    CHECK(!validate_product(bad, e));
  }
  auto bad = product;
  bad.back() ^= std::byte{1};
  CHECK(!validate_product(bad, entry));
  auto forged = entry;
  forged.revision = digest(bad);
  CHECK(!validate_product(bad, forged));
  // Failed products/catalog may leave orphans but cannot change active set.
  for (std::size_t failure = 0; failure <= inputs.size(); ++failure) {
    FakeFiles copy;
    copy.data = before;
    copy.fail_write = failure;
    auto changed = inputs;
    for (auto &i : changed)
      ++i.seed;
    CHECK(!cook(changed, copy));
    CHECK(copy.data["catalog.bin"] == before["catalog.bin"]);
    for (const auto &e : c.entries)
      CHECK(validate_product(copy.data[e.location], e));
  }
  Limits large;
  large.entries = 128;
  large.depth = 128;
  large.resources = 128;
  large.io_queue = 128;
  std::vector<Input> cycle;
  for (std::size_t i = 0; i < 128; ++i)
    cycle.push_back({aid(i + 1), 1, 1, 0, 1, {aid((i + 1) % 128 + 1)}, {}});
  Diagnostic causal;
  FakeFiles sink;
  CHECK_ERROR(cook(cycle, sink, large, &causal), ErrorCode::Cycle);
  CHECK(causal.path_size == 128);
  // Repeated diamonds previously cause exponential validator traversal.
  std::vector<Input> wide;
  for (std::size_t i = 0; i < 100; ++i) {
    Input x{aid(i + 1), 1, 1, 0, 1, {}, {}};
    if (i >= 2) {
      x.hard = {aid((i / 2 - 1) * 2 + 1), aid((i / 2 - 1) * 2 + 2)};
    }
    wide.push_back(x);
  }
  auto graph = cook(wide, sink, large);
  CHECK(graph);
  CHECK(validate_catalog(graph.value(), large));
  auto short_depth = large;
  short_depth.depth = 49;
  CHECK_ERROR(validate_catalog(graph.value(), short_depth), ErrorCode::Exhausted);
  std::string text =
      "RACEFIXTURE1 id " + id_text(aid(1)) +
      " seed 1 count 3 settings 0 environment 1 hard 0 soft 1 00000000000000000000000000000000";
  CHECK(parse_input(text));
  CHECK_ERROR(parse_input(text + " products 2"), ErrorCode::Unsupported);
  CHECK(!parse_input("RACEFIXTURE1 id zero"));
  CHECK(parse_id(id_text(aid(2))).value() == aid(2));
  // Root capability remains on its pinned directory after rename/replacement.
  // Linux-only directory-fd rename/symlink contract; Windows pinned-root
  // conformance is exercised in platform_windows_tests.cpp.
#ifndef _WIN32
  char name[] = "/tmp/race-assets-root-XXXXXX";
  CHECK(::mkdtemp(name));
  std::filesystem::path root(name);
  std::filesystem::create_directory(root / "content");
  auto rooted = platform::make_rooted_files((root / "content").string());
  CHECK(rooted);
  std::array<std::byte, 3> value{std::byte{1}, std::byte{2}, std::byte{3}};
  CHECK(rooted.value()->publish_immutable("one.bin", value));
  CHECK(rooted.value()->publish_immutable("one.bin", value));
  auto different = value;
  different[0] = std::byte{9};
  CHECK_ERROR(rooted.value()->publish_immutable("one.bin", different), ErrorCode::Duplicate);
  std::filesystem::rename(root / "content", root / "moved");
  std::filesystem::create_directory(root / "content");
  CHECK(rooted.value()->read("one.bin", 3).value() ==
        std::vector<std::byte>(value.begin(), value.end()));
  std::filesystem::create_symlink("/etc/passwd", root / "moved" / "escape.bin");
  CHECK(!rooted.value()->read("escape.bin", 4096));
  std::filesystem::create_directory_symlink(root / "moved", root / "linked");
  CHECK(!platform::make_rooted_files((root / "linked").string()));
  CHECK_ERROR(rooted.value()->replace("catalog.bin", value, platform::Durability::Durable),
              ErrorCode::Unsupported);
  CHECK(!rooted.value()->read("../one.bin", 3));
  CHECK_ERROR(rooted.value()->read("one.bin", 2), ErrorCode::Exhausted);
  rooted.value().reset();
  std::filesystem::remove_all(root);
#endif
#ifdef _WIN32
  std::printf("wire: deterministic shuffled cook; identity/type; hostile ranges; 128-node cycle; "
              "dense DAG; catalog-last faults; shared codec assertions passed; "
              "Windows root confinement checked by platform suite\n");
#else
  std::printf("wire: deterministic shuffled cook; identity/type; hostile ranges; 128-node cycle; "
              "dense DAG; catalog-last faults; rooted symlinks/rename passed\n");
#endif
}
