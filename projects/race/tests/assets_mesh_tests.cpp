#include "race/assets/mesh.hpp"
#include "test.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <thread>

void asset_fail_after(std::size_t);
void asset_fail_disable();
void asset_fail_payload_once(std::size_t);
namespace {
using namespace race;
namespace m = assets::mesh;
assets::Digest hash(std::string_view text) { return assets::digest(std::as_bytes(std::span(text))); }
m::Stamp stamp(std::string_view source = "mesh source v1") {
  m::Stamp s; s.source = hash(source); s.settings = hash("mesh settings v1"); s.importer = hash("mesh importer v1"); return s;
}
const std::array<m::Vertex, 3> vertices{{{{0, 0, 0}, {0, 0, 1}}, {{1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}}};
const std::array<std::uint32_t, 3> indices{0, 1, 2};
std::vector<std::byte> product(PersistentId id = {1, 2}, m::Stamp s = stamp()) {
  auto result = m::encode(id, s, vertices, indices); CHECK(result); return std::move(result).value();
}
void put(std::vector<std::byte> &bytes, std::size_t offset, std::uint64_t value, std::size_t n = 8) {
  for (std::size_t i = 0; i < n; ++i) bytes[offset + i] = std::byte((value >> (i * 8)) & 255);
}
void resign(std::vector<std::byte> &bytes) {
  auto checksum = assets::digest(std::span<const std::byte>(bytes).first(bytes.size() - m::checksum_bytes));
  std::copy(checksum.begin(), checksum.end(), bytes.end() - m::checksum_bytes);
}
std::unique_ptr<m::Store> store(TrackedAllocator allocator, m::Limits limits = {}) {
  auto result = m::Store::create(limits, allocator); CHECK(result); return std::move(result).value();
}
m::ResourceHandle publish(m::Store &s, const std::vector<std::byte> &bytes, m::Stamp provenance = stamp(), PersistentId id = {1, 2}) {
  auto update = s.begin_update(id, provenance); CHECK(update);
  auto result = s.publish(update.value(), bytes); CHECK(result); return result.value();
}
void wire_tests() {
  auto bytes = product();
  CHECK(bytes.size() == 444 && bytes == product());
  // Independently constructed with Python struct.pack little-endian fields and
  // hashlib; catches byte-layout drift beyond a self-consistent round trip.
  CHECK(assets::hex(assets::digest(bytes)) == "ffc311cbef778fbd8bbcfe5f92d1dcc7bc027edff590ea607d84e5b98b05eda4");
  auto decoded = m::validate(bytes); CHECK(decoded);
  CHECK(decoded.value().id() == PersistentId{1, 2});
  CHECK(decoded.value().stamp() == stamp());
  CHECK(decoded.value().revision() == assets::digest(bytes));
  CHECK(decoded.value().vertex_count() == 3 && decoded.value().index_count() == 3);
  CHECK(decoded.value().bounds() == m::Bounds{{0, 0, 0}, {1, 1, 0}});
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(decoded.value().vertex(i).value() == vertices[i]);
    CHECK(decoded.value().index(i).value() == indices[i]);
  }
  CHECK_ERROR(decoded.value().vertex(3), ErrorCode::InvalidInput);
  CHECK_ERROR(decoded.value().index(SIZE_MAX), ErrorCode::InvalidInput);
  asset_fail_after(0); auto allocation_free = m::validate(bytes); asset_fail_disable(); CHECK(allocation_free);
  for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!m::validate(std::span(bytes).first(n)));
  for (const auto at : {0U, 8U, 12U, 16U, 20U, 24U, 28U, 240U, 244U, 248U, 252U}) {
    auto bad = bytes; put(bad, at, UINT32_MAX, 4); resign(bad); CHECK(!m::validate(bad));
  }
  for (const auto at : {32U, 40U, 48U, 56U}) {
    auto bad = bytes; put(bad, at, UINT64_MAX); resign(bad); CHECK(!m::validate(bad));
  }
  auto bad = bytes; bad.push_back(std::byte{}); put(bad, 32, bad.size()); CHECK(!m::validate(bad));
  bad = bytes; put(bad, 64, 0); put(bad, 72, 0); resign(bad); CHECK(!m::validate(bad));
  for (const auto at : {80U, 112U, 144U, 176U, 208U}) {
    bad = bytes; bad[at] ^= std::byte{1}; CHECK(!m::validate(bad));
  }
  for (const auto value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e13}) {
    bad = bytes; put(bad, m::header_bytes, std::bit_cast<std::uint64_t>(value)); resign(bad); CHECK(!m::validate(bad));
  }
  bad = bytes; put(bad, m::header_bytes + 40, 0); resign(bad); CHECK(!m::validate(bad));
  bad = bytes; put(bad, m::header_bytes + 3 * m::vertex_stride, 3, 4); resign(bad); CHECK(!m::validate(bad));
  bad = bytes; put(bad, m::header_bytes + 3 * m::vertex_stride + 4, 0, 4); resign(bad); CHECK(!m::validate(bad));
  auto flat = vertices; flat[2].position = {2, 0, 0}; CHECK(!m::encode({1, 2}, stamp(), flat, indices));
  bad = bytes; put(bad, m::header_bytes + 2 * m::vertex_stride, std::bit_cast<std::uint64_t>(2.0));
  put(bad, m::header_bytes + 2 * m::vertex_stride + 8, 0); resign(bad); CHECK(!m::validate(bad));
  auto small = vertices; small[1].position[0] = 1e-200; small[2].position[1] = 1e-200;
  auto tiny = m::encode({1, 2}, stamp(), small, indices); CHECK(tiny && m::validate(tiny.value()));
  auto precise = vertices; precise[0].position[0] = 0x1.0000000000001p-5;
  auto exact = m::encode({1, 2}, stamp(), precise, indices); CHECK(exact);
  CHECK(m::validate(exact.value()).value().vertex(0).value() == precise[0]);
  CHECK_ERROR(m::encode({}, stamp(), vertices, indices), ErrorCode::InvalidInput);
  auto unknown = stamp(); unknown.target = 1; CHECK(!m::validate_stamp(unknown));
  auto cooked = stamp(); cooked.representation = m::Representation::Cooked;
  cooked.target = m::portable_cpu_target; cooked.cooker = hash("mesh cooker v1");
  auto cooked_bytes = product({1, 2}, cooked); CHECK(m::validate(cooked_bytes).value().stamp() == cooked);
  cooked.dependencies = hash("unsupported dependency"); CHECK(!m::validate_stamp(cooked));
  auto limits = m::Limits{}; limits.product_bytes = bytes.size(); CHECK(m::validate(bytes, limits));
  limits.product_bytes = bytes.size() - 1; CHECK(!m::validate(bytes, limits));
  limits = {}; limits.vertices = SIZE_MAX; CHECK(!m::validate(bytes, limits));
  // Hash success is not semantic admission: resigned corrupt geometry is rejected above.
}
void lifetime_tests() {
  auto allocation = TrackedAllocator::create({4096}); CHECK(allocation);
  auto allocator = allocation.value(); auto bytes = product();
  auto limits = m::Limits{}; limits.assets = 1; limits.retained_revisions = 2; limits.retained_bytes = bytes.size() * 2;
  auto s = store(allocator, limits);
  auto first = publish(*s, bytes);
  std::optional<m::Lease> old(s->acquire(first).value());
  bytes[m::header_bytes] ^= std::byte{127};
  CHECK(old->get().view().vertex(0).value() == vertices[0]); // Publication owns bytes.
  bytes = product(); auto newer = stamp("mesh source v2"); auto replacement = product({1, 2}, newer);
  auto obsolete = s->begin_update({1, 2}, stamp()); CHECK(obsolete);
  auto latest = s->begin_update({1, 2}, newer); CHECK(latest);
  CHECK_ERROR(s->publish(obsolete.value(), bytes), ErrorCode::StaleHandle);
  auto second = s->publish(latest.value(), replacement); CHECK(second);
  CHECK_ERROR(s->acquire(first), ErrorCode::StaleHandle);
  CHECK(s->acquire(second.value()).value()->view().stamp() == newer);
  CHECK(old->get().view().stamp() == stamp());
  CHECK(s->stats().retained_bytes == bytes.size() * 2 && allocator.snapshot().total.live == bytes.size() * 2);
  CHECK_ERROR(s->begin_update({9, 9}, stamp()), ErrorCode::Exhausted);
  auto full = s->begin_update({1, 2}, stamp()); CHECK(full);
  CHECK_ERROR(s->publish(full.value(), bytes), ErrorCode::Exhausted);
  CHECK_ERROR(s->publish(full.value(), bytes), ErrorCode::StaleHandle);
  CHECK(s->current({1, 2}).value() == second.value());
  old.reset(); CHECK(s->stats().retained_bytes == bytes.size());
  auto cancelled = s->begin_update({1, 2}, stamp()); CHECK(cancelled); CHECK(s->cancel(cancelled.value()));
  CHECK_ERROR(s->publish(cancelled.value(), bytes), ErrorCode::StaleHandle);
  auto pending = s->begin_update({1, 2}, stamp()); CHECK(pending);
  std::optional<m::Lease> survivor(s->acquire(second.value()).value());
  CHECK(s->retire({1, 2})); CHECK_ERROR(s->acquire(second.value()), ErrorCode::StaleHandle);
  CHECK_ERROR(s->publish(pending.value(), bytes), ErrorCode::StaleHandle);
  auto recreated = publish(*s, bytes); CHECK(recreated != first && recreated != second.value());
  CHECK_ERROR(s->publish(pending.value(), bytes), ErrorCode::StaleHandle);
  s.reset(); CHECK(allocator.snapshot().total.live == bytes.size());
  CHECK(survivor->get().view().stamp() == newer);
  survivor.reset(); CHECK(allocator.snapshot().total.live == 0);
  auto a = store(allocator), b = store(allocator);
  auto foreign = a->begin_update({1, 2}, stamp()); CHECK(foreign);
  CHECK_ERROR(b->publish(foreign.value(), bytes), ErrorCode::StaleHandle);
  auto handle = publish(*a, bytes);
  CHECK_ERROR(b->acquire(handle), ErrorCode::StaleHandle);
  std::thread reader([&] {
    CHECK(a->acquire(handle).value()->view().vertex(1).value() == vertices[1]);
    CHECK(a->stats().retained_revisions == 1);
    CHECK_ERROR(a->begin_update({1, 2}, stamp()), ErrorCode::WrongThread);
    CHECK_ERROR(a->publish(foreign.value(), bytes), ErrorCode::WrongThread);
    CHECK_ERROR(a->cancel(foreign.value()), ErrorCode::WrongThread);
    CHECK_ERROR(a->current({1, 2}), ErrorCode::WrongThread);
    CHECK_ERROR(a->retire({1, 2}), ErrorCode::WrongThread);
  }); reader.join();
  CHECK(a->retire({1, 2})); CHECK(allocator.snapshot().total.live == 0);
  limits.retained_bytes = bytes.size() - 1; auto tight = store(allocator, limits);
  auto denied = tight->begin_update({1, 2}, stamp()); CHECK(denied);
  CHECK_ERROR(tight->publish(denied.value(), bytes), ErrorCode::Exhausted);
  CHECK(tight->stats().retained_bytes == 0 && allocator.snapshot().total.live == 0);
  limits.retained_bytes = bytes.size(); tight = store(allocator, limits);
  CHECK(publish(*tight, bytes).table); tight.reset(); CHECK(allocator.snapshot().total.live == 0);
  limits.retained_bytes = 4096; limits.retained_revisions = 1; tight = store(allocator, limits);
  const auto pinned = publish(*tight, bytes);
  auto revision_full = tight->begin_update({1, 2}, stamp()); CHECK(revision_full);
  CHECK_ERROR(tight->publish(revision_full.value(), bytes), ErrorCode::Exhausted);
  CHECK(tight->current({1, 2}).value() == pinned && tight->stats().retained_revisions == 1);
  tight.reset(); CHECK(allocator.snapshot().total.live == 0);
}
void fault_tests() {
  auto allocation = TrackedAllocator::create({16384}); CHECK(allocation); auto allocator = allocation.value();
  const auto bytes = product();
  std::size_t failures{}, successes{};
  for (std::size_t n = 0; n < 24; ++n) {
    asset_fail_after(n); auto made = m::Store::create({}, allocator); asset_fail_disable();
    if (made) ++successes; else { CHECK(made.error().code == ErrorCode::Exhausted); ++failures; }
  }
  CHECK(failures && successes); failures = successes = 0;
  for (std::size_t n = 0; n < 24; ++n) {
    auto s = store(allocator); auto previous = publish(*s, bytes);
    auto update = s->begin_update({1, 2}, stamp()); CHECK(update);
    asset_fail_after(n); auto result = s->publish(update.value(), bytes); asset_fail_disable();
    if (result) { ++successes; CHECK_ERROR(s->acquire(previous), ErrorCode::StaleHandle); }
    else {
      ++failures; CHECK(result.error().code == ErrorCode::Exhausted);
      CHECK(s->current({1, 2}).value() == previous && s->acquire(previous));
    }
    CHECK(s->stats().retained_bytes == bytes.size() && s->stats().retained_revisions == 1);
    CHECK(allocator.snapshot().total.live == bytes.size());
    s.reset(); CHECK(allocator.snapshot().total.live == 0);
  }
  CHECK(failures && successes);
  auto s = store(allocator); const auto previous = publish(*s, bytes);
  for (int condition = 0; condition < 4; ++condition) {
    auto update = s->begin_update({1, 2}, stamp()); CHECK(update);
    auto bad = bytes;
    if (condition == 0) bad.back() ^= std::byte{1};
    if (condition == 1) bad = product({4, 5});
    if (condition == 2) bad = product({1, 2}, stamp("stale source"));
    if (condition == 3) asset_fail_payload_once(bytes.size());
    CHECK(!s->publish(update.value(), bad));
    CHECK(s->current({1, 2}).value() == previous && s->acquire(previous));
    CHECK(s->stats().retained_revisions == 1 && allocator.snapshot().total.live == bytes.size());
  }
  s.reset(); CHECK(allocator.snapshot().total.live == 0);
  asset_fail_after(0); auto encoded = m::encode({1, 2}, stamp(), vertices, indices); asset_fail_disable();
  CHECK(!encoded && encoded.error().code == ErrorCode::Exhausted);
}
} // namespace
void asset_mesh_tests() { wire_tests(); lifetime_tests(); fault_tests(); }
