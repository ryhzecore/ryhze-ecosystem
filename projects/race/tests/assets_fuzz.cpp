#include "assets_support.hpp"
#include <random>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  Limits limits;
  limits.entries = 16;
  limits.dependencies = 4;
  limits.depth = 8;
  limits.product_bytes = 65536;
  limits.catalog_bytes = 65536;
  limits.values = 4096;
  if (size > 65536)
    return 0;
  auto bytes = std::as_bytes(std::span(data, size));
  (void)decode_catalog(bytes, limits);
  if (size >= 128) {
    auto read = [&](std::size_t p) {
      std::uint64_t v = 0;
      for (std::size_t j = 0; j < 8; ++j)
        v |= std::uint64_t(data[p + j]) << (j * 8);
      return v;
    };
    Entry e;
    e.id = {read(40), read(48)};
    e.bytes = size;
    e.revision = digest(bytes);
    auto hc = read(104) & 0xffffffffULL;
    auto sc = read(104) >> 32;
    if (hc <= limits.dependencies && sc <= limits.dependencies && 128 + 48 * hc + 16 * sc <= size) {
      for (std::size_t i = 0; i < hc; ++i) {
        auto p = 128 + 48 * i;
        Dependency dep{{read(p), read(p + 8)}, {}};
        std::copy_n(bytes.begin() + p + 16, 32, dep.revision.begin());
        e.hard.push_back(dep);
      }
      for (std::size_t i = 0; i < sc; ++i) {
        auto p = 128 + 48 * hc + 16 * i;
        e.soft.push_back({read(p), read(p + 8)});
      }
    }
    (void)validate_product(bytes, e, limits);
  }
  if (size <= 4096)
    (void)parse_input(std::string_view(reinterpret_cast<const char *>(data), size), limits);
  return 0;
}
#ifndef RACE_LIBFUZZER
int main() {
  FakeFiles files;
  auto catalog = cook(diamond(), files);
  CHECK(catalog);
  std::mt19937 random(71923);
  std::size_t cases = 0;
  auto run = [&](std::span<const std::byte> b) {
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(b.data()), b.size());
    ++cases;
  };
  for (const auto &[name, bytes] : files.data) {
    (void)name;
    run(bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n)
      run(std::span(bytes).first(n));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      auto changed = bytes;
      changed[i] ^= std::byte{255};
      run(changed);
    }
    for (std::size_t at = 0; at + 8 <= bytes.size(); at += 8) {
      auto bad = bytes;
      std::fill_n(bad.begin() + at, 8, std::byte{255});
      run(bad);
    }
  }
  for (std::size_t i = 0; i < 1024; ++i) {
    std::vector<std::byte> input(random() % 2048);
    for (auto &b : input)
      b = std::byte(random() & 255);
    run(input);
  }
  std::printf(
      "bounded parser corpus cases=%zu seed=71923; callable LLVMFuzzerTestOneInput present\n",
      cases);
}
#endif
