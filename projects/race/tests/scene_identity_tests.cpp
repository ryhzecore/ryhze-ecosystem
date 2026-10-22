#include "race/foundation/identity.hpp"
#include "test.hpp"
#include <cstdint>
using namespace race;
int main() {
  {
    HandleTable<int, std::uint8_t> t(2), foreign(1);
    auto a = t.insert(std::make_shared<int>(1));
    auto b = t.insert(std::make_shared<int>(2));
    CHECK(a && b);
    auto old_scope = t.resolve(a.value());
    CHECK(old_scope);
    std::vector<Handle<int>> erased{a.value()};
    std::vector<std::shared_ptr<int>> owners{std::make_shared<int>(3)};
    auto replaced = t.replace_batch(erased, owners);
    CHECK(replaced);
    CHECK(replaced.value()[0].slot == a.value().slot);
    CHECK(replaced.value()[0].generation == 2);
    CHECK_ERROR(t.resolve(a.value()), ErrorCode::StaleHandle);
    CHECK(old_scope.value().get() == 1);
    CHECK(t.resolve(replaced.value()[0]).value().get() == 3);
    CHECK(t.resolve(b.value()).value().get() == 2);
    auto f = foreign.insert(std::make_shared<int>(4));
    CHECK(f);
    erased = {f.value()};
    CHECK_ERROR(t.replace_batch(erased, owners), ErrorCode::StaleHandle);
    erased = {b.value(), b.value()};
    CHECK_ERROR(t.replace_batch(erased, owners), ErrorCode::Duplicate);
    erased = {b.value()};
    owners = {nullptr};
    CHECK_ERROR(t.replace_batch(erased, owners), ErrorCode::InvalidInput);
    owners = {std::make_shared<int>(5), std::make_shared<int>(6)};
    CHECK_ERROR(t.replace_batch(erased, owners), ErrorCode::Exhausted);
    CHECK(t.resolve(b.value()).value().get() == 2);
    CHECK(t.available_slots() == 0);
  }
  HandleTable<int, std::uint8_t> table(1);
  CHECK(table.available_slots() == 1);
  CHECK_ERROR(table.insert({}), ErrorCode::InvalidInput);
  CHECK(table.available_slots() == 1);
  Handle<int> old{};
  for (unsigned generation = 1; generation <= 255; ++generation) {
    auto h = table.insert(std::make_shared<int>(7));
    CHECK(h);
    CHECK(h.value().generation == generation);
    CHECK(table.available_slots() == 0);
    if (generation > 1)
      CHECK_ERROR(table.resolve(old), ErrorCode::StaleHandle);
    auto lease = table.resolve(h.value());
    CHECK(lease);
    CHECK(table.erase(h.value()));
    CHECK(*lease.value().operator->() == 7);
    CHECK(table.available_slots() == (generation == 255 ? 0 : 1));
    old = h.value();
  }
  CHECK_ERROR(table.insert(std::make_shared<int>(8)), ErrorCode::Exhausted);
  CHECK(table.available_slots() == 0);
  {
    HandleTable<int, std::uint8_t> t(1);
    Handle<int> h;
    for (unsigned i = 1; i <= 255; ++i) {
      auto a = t.insert(std::make_shared<int>(9));
      CHECK(a);
      h = a.value();
      if (i < 255)
        CHECK(t.erase(h));
    }
    std::vector<Handle<int>> erased{h};
    std::vector<std::shared_ptr<int>> owners{std::make_shared<int>(10)};
    CHECK_ERROR(t.replace_batch(erased, owners), ErrorCode::Exhausted);
    CHECK(t.resolve(h).value().get() == 9); // Failed retirement never consumes live state.
    CHECK(t.replace_batch(erased, {}));
    CHECK(t.available_slots() == 0);
  }
  std::puts("scene_identity: passed");
}
