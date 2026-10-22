#include "mesh_pipeline.hpp"
#include "mesh_render.hpp"
#include "test.hpp"

void asset_fail_after(std::size_t);
void asset_fail_disable();
using namespace race;
void mesh_render_tests() {
  constexpr std::string_view source="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3";
  auto imported=mesh_pipeline::import_obj({88,1},std::as_bytes(std::span(source))); CHECK(imported);
  auto cooked=mesh_pipeline::cook(imported.value()); CHECK(cooked);
  auto allocator=TrackedAllocator::create({4096}); CHECK(allocator);
  auto store=assets::mesh::Store::create({},allocator.value()); CHECK(store);
  auto put=[&](const std::vector<std::byte>& data) {
    auto view=assets::mesh::validate(data); CHECK(view);
    auto update=store.value()->begin_update(view.value().id(),view.value().stamp()); CHECK(update);
    auto handle=store.value()->publish(update.value(),data); CHECK(handle);
    auto lease=store.value()->acquire(handle.value()); CHECK(lease); return std::move(lease).value();
  };
  CHECK_ERROR(desktop::copy_mesh_geometry(assets::mesh::Lease{nullptr}),ErrorCode::InvalidInput);
  auto imported_lease=put(imported.value());
  CHECK_ERROR(desktop::copy_mesh_geometry(imported_lease),ErrorCode::InvalidInput);
  auto lease=put(cooked.value());
  auto packet=desktop::copy_mesh_geometry(lease); CHECK(packet);
  CHECK(packet.value()->vertices().size()==3 && packet.value()->indices().size()==3);
  CHECK(packet.value()->vertices()[1].position==std::array<double,3>{1,0,0});
  bool success{}; std::size_t failures{};
  for (std::size_t n=0;n<32 && !success;++n) {
    asset_fail_after(n); auto candidate=desktop::copy_mesh_geometry(lease); asset_fail_disable();
    if (candidate) success=true;
    else { CHECK(candidate.error().code==ErrorCode::Exhausted); ++failures; }
  }
  CHECK(success && failures>=5);
  CHECK(store.value()->retire({88,1})); store.value().reset();
  lease=assets::mesh::Lease{nullptr}; imported_lease=assets::mesh::Lease{nullptr};
  imported.value().clear(); cooked.value().clear();
  CHECK(packet.value()->vertices()[2].position==std::array<double,3>{0,1,0});
  CHECK(packet.value()->indices()[2]==2);
}
