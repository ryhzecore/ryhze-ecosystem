#include "race/assets/assets.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <sstream>

namespace race::assets {
namespace {
bool less_id(AssetId a, AssetId b) {
  return a.high < b.high || (a.high == b.high && a.low < b.low);
}
std::uint64_t u(std::span<const std::byte> b, std::size_t p, std::size_t n = 8) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i)
    v |= std::uint64_t(std::to_integer<unsigned char>(b[p + i])) << (i * 8);
  return v;
}
void put(std::vector<std::byte> &b, std::size_t p, std::uint64_t v, std::size_t n = 8) {
  for (std::size_t i = 0; i < n; ++i)
    b[p + i] = std::byte((v >> (i * 8)) & 255);
}
AssetId get_id(std::span<const std::byte> b, std::size_t p) { return {u(b, p), u(b, p + 8)}; }
void put_id(std::vector<std::byte> &b, std::size_t p, AssetId id) {
  put(b, p, id.high);
  put(b, p + 8, id.low);
}
bool magic(std::span<const std::byte> b, const char *s) {
  return b.size() >= 8 && !std::memcmp(b.data(), s, 8);
}
void set_magic(std::vector<std::byte> &b, const char *s) { std::memcpy(b.data(), s, 8); }
Result<void> diag(Diagnostic *d, AssetId a, AssetId cause, ErrorCode e, const char *m) {
  if (d) {
    d->asset = a;
    d->cause = cause;
    d->code = e;
  }
  return fail(e, m);
}
std::size_t find(const Catalog &c, AssetId id) {
  auto it = std::lower_bound(c.entries.begin(), c.entries.end(), id,
                             [](const Entry &e, AssetId x) { return less_id(e.id, x); });
  return it != c.entries.end() && it->id == id ? static_cast<std::size_t>(it - c.entries.begin())
                                               : c.entries.size();
}
} // namespace
std::string hex(Digest d) {
  constexpr char a[] = "0123456789abcdef";
  std::string r(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    auto v = std::to_integer<unsigned>(d[i]);
    r[2 * i] = a[v >> 4];
    r[2 * i + 1] = a[v & 15];
  }
  return r;
}
std::string id_text(AssetId id) {
  constexpr char a[] = "0123456789abcdef";
  std::string r(32, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    r[15 - i] = a[(id.high >> (i * 4)) & 15];
    r[31 - i] = a[(id.low >> (i * 4)) & 15];
  }
  return r;
}
Result<AssetId> parse_id(std::string_view s) noexcept {
  if (s.size() != 32)
    return fail(ErrorCode::InvalidInput, "identity requires 32 lowercase hex digits");
  AssetId id;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return fail(ErrorCode::InvalidInput, "invalid identity digit");
    auto &part = i < 16 ? id.high : id.low;
    part = (part << 4) | static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  return id;
}
Result<void> validate_limits(const Limits &l) noexcept {
  if (!l.entries || l.entries > hard_max_entries || !l.dependencies ||
      l.dependencies > hard_max_dependencies || !l.depth || l.depth > hard_max_entries ||
      !l.values || l.values > 1024 * 1024 || l.product_bytes < 128 ||
      l.product_bytes > 16 * 1024 * 1024 || l.catalog_bytes < 64 ||
      l.catalog_bytes > 4 * 1024 * 1024 || !l.requests || l.requests > 4096 || !l.resources ||
      l.resources > hard_max_entries || !l.io_queue || l.io_queue > hard_max_entries ||
      !l.decode_jobs || l.decode_jobs > hard_max_entries || !l.temporary_bytes ||
      l.temporary_bytes > 1024ULL * 1024 * 1024 || !l.retained_bytes ||
      l.retained_bytes > 1024ULL * 1024 * 1024 || !l.retained_revisions ||
      l.retained_revisions > 4096)
    return fail(ErrorCode::InvalidInput, "invalid bounded asset limits");
  return {};
}
Result<void> validate_catalog(const Catalog &c, const Limits &l, Diagnostic *d) {
  try {
    auto lim = validate_limits(l);
    if (!lim)
      return lim;
    if (d)
      *d = {};
    if (c.entries.size() > l.entries)
      return fail(ErrorCode::Exhausted, "catalog entry limit");
    for (std::size_t i = 1; i < c.entries.size(); ++i)
      if (!less_id(c.entries[i - 1].id, c.entries[i].id))
        return diag(d, c.entries[i].id, c.entries[i - 1].id, ErrorCode::Duplicate,
                    "catalog identity order");
    for (std::size_t i = 0; i < c.entries.size(); ++i) {
      const auto &e = c.entries[i];
      if (!e.id.valid())
        return diag(d, e.id, {}, ErrorCode::InvalidInput, "zero required asset");
      if (i && !less_id(c.entries[i - 1].id, e.id))
        return diag(d, e.id, e.id, ErrorCode::Duplicate,
                    "catalog identities not unique and sorted");
      if (e.type != fixture_type || e.target != linux_target || e.schema != 1)
        return diag(d, e.id, {}, ErrorCode::IncompatibleVersion,
                    "unsupported product type target or schema");
      if (e.bytes < 128 || e.bytes > l.product_bytes)
        return diag(d, e.id, {}, ErrorCode::Exhausted, "product size limit");
      if (!platform::valid_content_leaf(e.location) || e.location != hex(e.revision) + ".bin")
        return diag(d, e.id, {}, ErrorCode::InvalidInput, "noncanonical product location");
      for (std::size_t j = 0; j < i; ++j)
        if (c.entries[j].location == e.location)
          return diag(d, e.id, c.entries[j].id, ErrorCode::Duplicate, "duplicate product location");
      if (e.hard.size() > l.dependencies || e.soft.size() > l.dependencies)
        return diag(d, e.id, {}, ErrorCode::Exhausted, "dependency count limit");
      for (std::size_t j = 0; j < e.hard.size(); ++j) {
        auto dep = e.hard[j].id;
        if (!dep.valid())
          return diag(d, e.id, dep, ErrorCode::InvalidDependency, "zero hard dependency");
        if (j && !less_id(e.hard[j - 1].id, dep))
          return diag(d, e.id, dep, ErrorCode::Duplicate, "hard dependency order");
        auto idx = find(c, dep);
        if (idx == c.entries.size())
          return diag(d, e.id, dep, ErrorCode::NotFound, "missing hard dependency");
        if (e.hard[j].revision != c.entries[idx].revision)
          return diag(d, e.id, dep, ErrorCode::IncompatibleVersion, "dependency revision mismatch");
      }
      for (std::size_t j = 1; j < e.soft.size(); ++j)
        if (!less_id(e.soft[j - 1], e.soft[j]))
          return diag(d, e.id, e.soft[j], ErrorCode::Duplicate, "soft dependency order");
    }
    std::array<std::size_t, hard_max_entries> path{}, longest{};
    std::array<unsigned char, hard_max_entries> color{};
    auto visit = [&](auto &&self, std::size_t index, std::size_t depth) -> Result<void> {
      if (color[index] == 1) {
        if (d) {
          d->path_size = std::min(depth + 1, hard_max_entries);
          for (std::size_t j = 0; j < d->path_size; ++j)
            d->path[j] = j < depth ? c.entries[path[j]].id : c.entries[index].id;
        }
        return diag(d, c.entries[path[0]].id, c.entries[index].id, ErrorCode::Cycle,
                    "hard dependency cycle");
      }
      if (color[index] == 2)
        return {};
      if (depth >= hard_max_entries)
        return fail(ErrorCode::Exhausted, "dependency traversal limit");
      color[index] = 1;
      path[depth] = index;
      longest[index] = 1;
      for (auto dep : c.entries[index].hard) {
        auto child = find(c, dep.id);
        auto r = self(self, child, depth + 1);
        if (!r)
          return r;
        longest[index] = std::max(longest[index], 1 + longest[child]);
      }
      if (longest[index] > l.depth)
        return diag(d, c.entries[index].id, {}, ErrorCode::Exhausted, "dependency depth limit");
      color[index] = 2;
      return {};
    };
    for (std::size_t i = 0; i < c.entries.size(); ++i) {
      auto r = visit(visit, i, 0);
      if (!r)
        return r;
    }
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "catalog validation allocation");
  }
}
Result<std::vector<std::byte>> encode_catalog(const Catalog &c, const Limits &l) {
  try {
    auto v = validate_catalog(c, l);
    if (!v)
      return v.error();
    std::size_t size = 64 + c.entries.size() * 176;
    for (const auto &e : c.entries)
      size += e.hard.size() * 48 + e.soft.size() * 16;
    if (size > l.catalog_bytes)
      return fail(ErrorCode::Exhausted, "catalog byte limit");
    std::vector<std::byte> b(size);
    set_magic(b, "RACECAT1");
    put(b, 8, 1, 4);
    put(b, 16, 64, 4);
    put(b, 20, c.entries.size(), 4);
    put(b, 24, 176, 4);
    put(b, 32, size);
    put(b, 40, 64 + c.entries.size() * 176);
    std::size_t at = 64 + c.entries.size() * 176;
    for (std::size_t i = 0; i < c.entries.size(); ++i) {
      const auto &e = c.entries[i];
      auto p = 64 + i * 176;
      put_id(b, p, e.id);
      put(b, p + 16, e.type, 4);
      put(b, p + 20, e.target, 4);
      put(b, p + 24, e.schema, 4);
      put(b, p + 32, e.bytes);
      std::copy(e.revision.begin(), e.revision.end(), b.begin() + p + 40);
      std::memcpy(b.data() + p + 72, e.location.data(), e.location.size());
      put(b, p + 144, e.hard.size(), 4);
      put(b, p + 148, e.soft.size(), 4);
      put(b, p + 152, at);
      for (auto dep : e.hard) {
        put_id(b, at, dep.id);
        std::copy(dep.revision.begin(), dep.revision.end(), b.begin() + at + 16);
        at += 48;
      }
      put(b, p + 160, at);
      for (auto id : e.soft) {
        put_id(b, at, id);
        at += 16;
      }
    }
    return b;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "catalog encode allocation");
  }
}
Result<Catalog> decode_catalog(std::span<const std::byte> b, const Limits &l, Diagnostic *d) {
  if (d)
    *d = {};
  auto lim = validate_limits(l);
  if (!lim)
    return lim.error();
  if (b.size() < 64 || b.size() > l.catalog_bytes || !magic(b, "RACECAT1"))
    return fail(ErrorCode::InvalidInput, "catalog header or size");
  if (u(b, 8, 4) != 1 || u(b, 12, 4) || u(b, 16, 4) != 64 || u(b, 24, 4) != 176 || u(b, 28, 4) ||
      u(b, 48) || u(b, 56))
    return fail(ErrorCode::IncompatibleVersion, "catalog format or mandatory features");
  auto count = u(b, 20, 4);
  if (count > l.entries || u(b, 32) != b.size() || u(b, 40) != 64 + count * 176 ||
      64 + count * 176 > b.size())
    return fail(ErrorCode::InvalidInput, "catalog record range");
  // Validate every range before allocating any catalog entries or strings.
  std::size_t at = 64 + count * 176;
  for (std::size_t i = 0; i < count; ++i) {
    auto p = 64 + i * 176;
    auto hc = u(b, p + 144, 4), sc = u(b, p + 148, 4);
    if (hc > l.dependencies || sc > l.dependencies || u(b, p + 28, 4) || u(b, p + 168) ||
        u(b, p + 152) != at || hc * 48 > b.size() - at)
      return fail(ErrorCode::InvalidInput, "catalog hard section range");
    at += hc * 48;
    if (u(b, p + 160) != at || sc * 16 > b.size() - at)
      return fail(ErrorCode::InvalidInput, "catalog soft section range");
    at += sc * 16;
    for (std::size_t j = 68; j < 72; ++j)
      if (b[p + 72 + j] != std::byte{})
        return fail(ErrorCode::InvalidInput, "catalog location padding");
  }
  if (at != b.size())
    return fail(ErrorCode::InvalidInput, "catalog trailing bytes");
  try {
    Catalog c;
    c.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      auto p = 64 + i * 176;
      Entry e;
      e.id = get_id(b, p);
      e.type = static_cast<std::uint32_t>(u(b, p + 16, 4));
      e.target = static_cast<std::uint32_t>(u(b, p + 20, 4));
      e.schema = static_cast<std::uint32_t>(u(b, p + 24, 4));
      e.bytes = u(b, p + 32);
      std::copy_n(b.begin() + p + 40, 32, e.revision.begin());
      e.location.assign(reinterpret_cast<const char *>(b.data() + p + 72), 68);
      for (std::size_t j = 0; j < u(b, p + 144, 4); ++j) {
        auto q = u(b, p + 152) + j * 48;
        Dependency dep{get_id(b, q), {}};
        std::copy_n(b.begin() + q + 16, 32, dep.revision.begin());
        e.hard.push_back(dep);
      }
      for (std::size_t j = 0; j < u(b, p + 148, 4); ++j)
        e.soft.push_back(get_id(b, u(b, p + 160) + j * 16));
      c.entries.push_back(std::move(e));
    }
    auto check = validate_catalog(c, l, d);
    if (!check)
      return check.error();
    return c;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "catalog decode allocation");
  }
}
std::uint64_t fixture_value(std::uint64_t seed, std::uint64_t index) noexcept {
  auto x = seed + 0x9e3779b97f4a7c15ULL * (index + 1);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
Result<ProductView> validate_product(std::span<const std::byte> b, const Entry &e,
                                     const Limits &l) {
  auto lim = validate_limits(l);
  if (!lim)
    return lim.error();
  if (b.size() < 128 || b.size() > l.product_bytes || b.size() != e.bytes || !magic(b, "RACEAS01"))
    return fail(ErrorCode::InvalidInput, "product size or magic");
  if (u(b, 8, 4) != 1 || u(b, 12, 4) || u(b, 16, 4) != fixture_type ||
      u(b, 20, 4) != linux_target || u(b, 24, 4) != 1 || u(b, 28, 4) != 128 || u(b, 88) != 1 ||
      u(b, 96) != 1 || e.type != fixture_type || e.target != linux_target || e.schema != 1)
    return fail(ErrorCode::IncompatibleVersion, "product format type target schema or tool");
  auto hc = u(b, 104, 4), sc = u(b, 108, 4), count = u(b, 64);
  std::uint64_t offset = 128 + hc * 48 + sc * 16;
  if (u(b, 32) != b.size() || get_id(b, 40) != e.id || !e.id.valid() || hc > l.dependencies ||
      sc > l.dependencies || hc != e.hard.size() || sc != e.soft.size() || !count ||
      count > l.values || u(b, 112) != 128 || u(b, 120) != offset || offset > b.size() ||
      count > (b.size() - offset) / 8 || offset + count * 8 != b.size())
    return fail(ErrorCode::InvalidInput, "product identity or section ranges");
  if (digest(b) != e.revision)
    return fail(ErrorCode::InvalidInput, "product digest mismatch");
  for (std::size_t i = 0; i < hc; ++i) {
    if (!e.hard[i].id.valid() || (i && !less_id(e.hard[i - 1].id, e.hard[i].id)))
      return fail(ErrorCode::InvalidDependency, "noncanonical hard dependency");
    auto p = 128 + i * 48;
    if (get_id(b, p) != e.hard[i].id ||
        !std::equal(e.hard[i].revision.begin(), e.hard[i].revision.end(), b.begin() + p + 16))
      return fail(ErrorCode::InvalidDependency, "product hard dependency mismatch");
  }
  for (std::size_t i = 0; i < sc; ++i)
    if ((i && !less_id(e.soft[i - 1], e.soft[i])) || get_id(b, 128 + hc * 48 + i * 16) != e.soft[i])
      return fail(ErrorCode::InvalidDependency, "product soft dependency mismatch");
  auto seed = u(b, 56);
  for (std::size_t i = 0; i < count; ++i)
    if (u(b, offset + i * 8) != fixture_value(seed, i))
      return fail(ErrorCode::InvalidInput, "fixture payload mismatch");
  return ProductView{e.id, seed, count, u(b, 72), u(b, 80), b.subspan(offset)};
}
Result<Input> parse_input(std::string_view text, const Limits &l) {
  auto lim = validate_limits(l);
  if (!lim)
    return lim.error();
  if (text.size() > 4096)
    return fail(ErrorCode::Exhausted, "readable fixture limit");
  auto token = [&]() {
    auto whitespace = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
    while (!text.empty() && whitespace(text.front()))
      text.remove_prefix(1);
    std::size_t n = 0;
    while (n < text.size() && !whitespace(text[n]))
      ++n;
    auto out = text.substr(0, n);
    text.remove_prefix(n);
    return out;
  };
  auto number = [&]() -> Result<std::uint64_t> {
    auto t = token();
    std::uint64_t v{};
    auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (t.empty() || ec != std::errc{} || end != t.data() + t.size())
      return fail(ErrorCode::InvalidInput, "fixture unsigned number invalid");
    return v;
  };
  try {
    Input out;
    if (token() != "RACEFIXTURE1")
      return fail(ErrorCode::IncompatibleVersion, "fixture input version");
    const char *keys[] = {"id", "seed", "count", "settings", "environment", "hard", "soft"};
    for (int field = 0; field < 7; ++field) {
      if (token() != keys[field])
        return fail(ErrorCode::InvalidInput, "fixture field order");
      if (field == 0) {
        auto id = parse_id(token());
        if (!id || !id.value().valid())
          return fail(ErrorCode::InvalidInput, "fixture identity invalid");
        out.id = id.value();
      } else {
        auto n = number();
        if (!n)
          return n.error();
        if (field == 1)
          out.seed = n.value();
        if (field == 2)
          out.count = n.value();
        if (field == 3)
          out.settings = n.value();
        if (field == 4)
          out.environment = n.value();
        if (field >= 5) {
          if (n.value() > l.dependencies)
            return fail(ErrorCode::Exhausted, "fixture dependency count");
          auto &deps = field == 5 ? out.hard : out.soft;
          deps.reserve(n.value());
          for (std::size_t i = 0; i < n.value(); ++i) {
            auto id = parse_id(token());
            if (!id)
              return id.error();
            deps.push_back(id.value());
          }
        }
      }
    }
    if (!token().empty())
      return fail(ErrorCode::Unsupported, "unsupported fixture fields or products");
    if (!out.count || out.count > l.values)
      return fail(ErrorCode::Exhausted, "fixture value limit");
    return out;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "fixture input allocation");
  }
}
Result<Catalog> cook(std::span<const Input> inputs, platform::RootedFiles &files, const Limits &l,
                     Diagnostic *d) {
  if (d)
    *d = {};
  auto lim = validate_limits(l);
  if (!lim)
    return lim.error();
  if (inputs.size() > l.entries)
    return fail(ErrorCode::Exhausted, "cook entry limit");
  for (const auto &x : inputs)
    if (!x.id.valid() || !x.count || x.count > l.values || x.hard.size() > l.dependencies ||
        x.soft.size() > l.dependencies)
      return fail(ErrorCode::InvalidInput, "invalid cook input");
  try {
    std::vector<Input> sorted(inputs.begin(), inputs.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const Input &a, const Input &b) { return less_id(a.id, b.id); });
    Catalog c;
    c.entries.resize(sorted.size());
    for (std::size_t i = 0; i < sorted.size(); ++i) {
      auto &x = sorted[i];
      if (!x.id.valid() || !x.count || x.count > l.values || x.hard.size() > l.dependencies ||
          x.soft.size() > l.dependencies)
        return fail(ErrorCode::InvalidInput, "invalid cook input");
      if (i && sorted[i - 1].id == x.id)
        return fail(ErrorCode::Duplicate, "duplicate cook identity");
      std::sort(x.hard.begin(), x.hard.end(), less_id);
      std::sort(x.soft.begin(), x.soft.end(), less_id);
      c.entries[i].id = x.id;
      for (auto id : x.hard)
        c.entries[i].hard.push_back({id, {}});
      c.entries[i].soft = x.soft;
      c.entries[i].bytes = 128 + 48 * x.hard.size() + 16 * x.soft.size() + 8 * x.count;
      c.entries[i].location = hex({}) + ".bin";
    }
    // Products may be orphaned on later failure; only catalog-last activates them.
    std::array<unsigned char, hard_max_entries> mark{};
    std::array<AssetId, hard_max_entries> path{};
    auto emit = [&](auto &&self, std::size_t index, std::size_t depth) -> Result<void> {
      auto &e = c.entries[index];
      auto &x = sorted[index];
      if (mark[index] == 1) {
        if (d) {
          *d = {};
          d->path_size = std::min(depth + 1, hard_max_entries);
          std::copy_n(path.begin(), std::min(depth, hard_max_entries), d->path.begin());
          if (depth < hard_max_entries)
            d->path[depth] = e.id;
        }
        return diag(d, path[0], e.id, ErrorCode::Cycle, "cook hard cycle");
      }
      if (depth >= l.depth)
        return diag(d, e.id, {}, ErrorCode::Exhausted, "cook depth limit");
      if (mark[index] == 2)
        return {};
      mark[index] = 1;
      path[depth] = e.id;
      for (std::size_t j = 0; j < e.hard.size(); ++j) {
        auto &dep = e.hard[j];
        if (!dep.id.valid() || (j && dep.id == e.hard[j - 1].id))
          return diag(d, e.id, dep.id, ErrorCode::InvalidDependency, "invalid hard dependency");
        auto di = find(c, dep.id);
        if (di == c.entries.size())
          return diag(d, e.id, dep.id, ErrorCode::NotFound, "missing cook dependency");
        auto r = self(self, di, depth + 1);
        if (!r)
          return r;
        dep.revision = c.entries[di].revision;
      }
      for (std::size_t j = 1; j < e.soft.size(); ++j)
        if (e.soft[j - 1] == e.soft[j])
          return fail(ErrorCode::Duplicate, "duplicate soft reference");
      if (e.bytes > l.product_bytes)
        return fail(ErrorCode::Exhausted, "cook product byte limit");
      std::vector<std::byte> b(e.bytes);
      set_magic(b, "RACEAS01");
      put(b, 8, 1, 4);
      put(b, 16, fixture_type, 4);
      put(b, 20, linux_target, 4);
      put(b, 24, 1, 4);
      put(b, 28, 128, 4);
      put(b, 32, b.size());
      put_id(b, 40, x.id);
      put(b, 56, x.seed);
      put(b, 64, x.count);
      put(b, 72, x.settings);
      put(b, 80, x.environment);
      put(b, 88, 1);
      put(b, 96, 1);
      put(b, 104, e.hard.size(), 4);
      put(b, 108, e.soft.size(), 4);
      put(b, 112, 128);
      std::size_t at = 128;
      for (auto dep : e.hard) {
        put_id(b, at, dep.id);
        std::copy(dep.revision.begin(), dep.revision.end(), b.begin() + at + 16);
        at += 48;
      }
      for (auto id : e.soft) {
        put_id(b, at, id);
        at += 16;
      }
      put(b, 120, at);
      for (std::size_t j = 0; j < x.count; ++j)
        put(b, at + j * 8, fixture_value(x.seed, j));
      e.revision = digest(b);
      e.location = hex(e.revision) + ".bin";
      auto valid = validate_product(b, e, l);
      if (!valid)
        return valid.error();
      auto written = files.publish_immutable(e.location, b);
      if (!written)
        return written;
      auto stored = files.read(e.location, l.product_bytes);
      if (!stored)
        return stored.error();
      auto check = validate_product(stored.value(), e, l);
      if (!check)
        return check.error();
      mark[index] = 2;
      return {};
    };
    for (std::size_t i = 0; i < sorted.size(); ++i) {
      auto r = emit(emit, i, 0);
      if (!r)
        return r.error();
    }
    auto bytes = encode_catalog(c, l);
    if (!bytes)
      return bytes.error();
    auto checked = decode_catalog(bytes.value(), l, d);
    if (!checked)
      return checked.error();
    auto published =
        files.replace("catalog.bin", bytes.value(), platform::Durability::AtomicVisibility);
    if (!published)
      return published.error();
    return c;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "cook allocation");
  }
}
} // namespace race::assets
