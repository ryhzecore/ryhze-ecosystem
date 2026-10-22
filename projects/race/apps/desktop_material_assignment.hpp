#pragma once
#include "desktop_meshes.hpp"
#include <thread>

namespace race::desktop {
struct MaterialAssignmentCompletion {
  Result<void> result;
  MaterialReference reference;
  std::shared_ptr<const MeshSet> prepared;
};
// One request and worker. Input and candidate packets remain owned until join.
// Cancellation suppresses publication; it does not imply I/O quiescence.
class MaterialAssignmentTask {
  struct Pending;
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
  const std::thread::id owner_{std::this_thread::get_id()};
  bool cancelled_{};
public:
  MaterialAssignmentTask();
  ~MaterialAssignmentTask();
  bool busy() const { return worker_.joinable(); }
  Result<void> start(const Document&, const ObjectRow&, std::shared_ptr<const MeshSet>, std::string leaf);
  Result<void> cancel();
  // Owner admits both Scene transaction and prepared cache together. User text
  // or a transient transform preview can veto publication without changing Scene.
  Result<std::optional<MaterialAssignmentCompletion>> poll(Document&, PersistentId selection, bool allow_publication);
};
}
