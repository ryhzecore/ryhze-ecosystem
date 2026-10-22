#include "race/scene/scene.hpp"
#include "test.hpp"
#include <cstdlib>
#include <limits>
#include <new>
// Owner-thread failure injection. TSan owns strong allocation operators, so its
// build uses executable link wrappers and the real sanitizer allocator. Other
// builds retain global overrides, also exercising shared-library stream growth.
// Link wrappers cannot intercept allocations already bound inside shared libraries.
extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__real__Znam(std::size_t);
namespace {
thread_local std::size_t remaining = std::numeric_limits<std::size_t>::max();
}
namespace {
void allocation_point() {
  if (remaining != std::numeric_limits<std::size_t>::max()) {
    if (!remaining) {
      remaining = std::numeric_limits<std::size_t>::max();
      throw std::bad_alloc();
    }
    --remaining;
  }
}
} // namespace
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(thread_sanitizer)
extern "C" void *__wrap__Znwm(std::size_t bytes) {
  allocation_point();
  return __real__Znwm(bytes);
}
extern "C" void *__wrap__Znam(std::size_t bytes) {
  allocation_point();
  return __real__Znam(bytes);
}
#else
void *operator new(std::size_t bytes) {
  allocation_point();
  if (auto *p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
#endif
void scene_allocation_failures() {
  using namespace race;
  using namespace race::scene;
  auto schema = Schemas::create();
  CHECK(schema);
  Document d;
  d.objects.push_back({{1, 1}, {}, {transform_component()}, {}});
  auto expected = encode_json(d, *schema.value(), {});
  CHECK(expected);
  std::size_t failed = 0, passed = 0;
  for (std::size_t point = 0; point < 128; ++point) {
    remaining = point;
    auto result = encode_json(d, *schema.value(), {});
    remaining = std::numeric_limits<std::size_t>::max();
    if (result) {
      CHECK(result.value() == expected.value());
      CHECK(decode_json(result.value(), *schema.value(), {}));
      ++passed;
    } else {
      CHECK(result.error().code == ErrorCode::Exhausted);
      ++failed;
    }
  }
  CHECK(failed > 0 && passed > 0);
  auto allocator = TrackedAllocator::create({16 * 1024 * 1024});
  CHECK(allocator);
  Limits limits;
  limits.objects = 4;
  limits.components = 8;
  limits.template_objects = 4;
  auto world = World::create(allocator.value(), schema.value(), limits);
  CHECK(world);
  CHECK(world.value()->apply({{Command::create(d.objects[0])}}));
  auto original = world.value()->document();
  CHECK(original);
  Transaction edit{{Command::set({1, 1}, 1, 1, Vec3{9, 0, 0}),
                    Command::create({{1, 2}, {}, {transform_component()}, {}})}};
  failed = passed = 0;
  for (std::size_t point = 0; point < 128; ++point) {
    remaining = point;
    auto result = world.value()->apply(edit);
    remaining = std::numeric_limits<std::size_t>::max();
    if (result) {
      ++passed;
      CHECK(world.value()->undo());
      CHECK(world.value()->clear_history());
    } else {
      ++failed;
      CHECK(result.error().code == ErrorCode::Exhausted);
    }
    auto after = world.value()->document();
    CHECK(after && after.value().objects == original.value().objects);
  }
  CHECK(failed > 0 && passed > 0);
  Document replacement = original.value();
  replacement.origin.position = {42, 0, 0};
  SectionInfo section;
  section.definition.id = {8, 8};
  section.state = SectionState::Active;
  section.manifest = {{1, 1}, {1, 3}};
  replacement.sections.push_back(section);
  replacement.objects[0].section = section.definition.id;
  auto added = replacement.objects[0];
  added.id = {1, 3};
  replacement.objects.push_back(added);
  failed = passed = 0;
  for (std::size_t point = 0; point < 192; ++point) {
    auto before = world.value()->document();
    CHECK(before);
    auto before_text = encode_json(before.value(), *schema.value(), limits);
    CHECK(before_text);
    auto before_stats = world.value()->stats();
    auto view = world.value()->read();
    CHECK(view);
    auto handle = view.value().find({1, 1});
    CHECK(handle);
    auto origin = view.value().origin();
    view.value().release();
    remaining = point;
    auto result = world.value()->load(replacement);
    remaining = std::numeric_limits<std::size_t>::max();
    if (result) {
      ++passed;
      CHECK(world.value()->load(original.value()));
    } else {
      ++failed;
      CHECK(result.error().code == ErrorCode::Exhausted);
      CHECK(world.value()->stats().generation == before_stats.generation);
      CHECK(world.value()->stats().history_groups == before_stats.history_groups);
      auto current = world.value()->document();
      CHECK(current);
      CHECK(encode_json(current.value(), *schema.value(), limits).value() == before_text.value());
      auto retained = world.value()->read();
      CHECK(retained);
      CHECK(retained.value().object(handle.value()));
      CHECK(retained.value().origin().revision == origin.revision);
    }
  }
  CHECK(failed > 0 && passed > 0);
}
