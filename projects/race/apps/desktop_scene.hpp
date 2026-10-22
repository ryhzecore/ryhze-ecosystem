#pragma once
#include "race/render/light_output.hpp"
#include "race/scene/scene.hpp"
#include <mutex>

namespace race::desktop {
// App composition schema; portable Scene owns storage, history and validation.
inline constexpr scene::TypeId box_type = 2, mesh_type = 3, material_type = 4, light_type = 5, output_type = 6;
// P03.04 composition settings (docs/progress/p03-light-output-contracts-20261013.md).
// Both components live on one dedicated settings object; at most one of each exists in a
// Scene. Missing settings mean LegacyViewportV1 frames; nothing infers lux or nits.
struct DirectionalLightSettings {
  scene::Vec3 direction{.4,.8,.3}; // World direction toward the light; normalised on admission.
  double illuminance_lux{500};      // Normal-incidence illuminance of the white D65 light.
  bool enabled{true};
  friend bool operator==(const DirectionalLightSettings&,const DirectionalLightSettings&)=default;
};
struct RenderOutputSettings {
  render::LightingProfile profile{render::LightingProfile::PhysicalDiffuseV1};
  double environment_cd_m2{20}, exposure_stops{0}, reference_luminance_cd_m2{100};
  render::OutputTarget target{render::OutputTarget::DisplaySdr};
  double paper_white_nits{200}, peak_nits{1000}; // Reference HDR curve parameters; presentation stays open.
  scene::Vec3 background_cd_m2{3,4,6.4};
  friend bool operator==(const RenderOutputSettings&,const RenderOutputSettings&)=default;
};
struct LightingSettings {
  std::optional<DirectionalLightSettings> light;
  std::optional<RenderOutputSettings> output;
  PersistentId light_object, output_object; // Owning settings object identities; empty when absent.
  friend bool operator==(const LightingSettings&,const LightingSettings&)=default;
};
// Application semantics beyond generic field validation: finite nonzero direction,
// declared photometric bounds, paper white within peak, one light and one output
// component per Scene. Validated on every load, install, publication and edit.
Result<void> validate_lighting(const LightingSettings&);
Result<LightingSettings> lighting_settings(const scene::Document&);
struct MeshReference {
  PersistentId asset;
  std::array<std::byte,32> revision{};
  bool valid() const noexcept;
  friend bool operator==(const MeshReference&,const MeshReference&)=default;
};
using MaterialReference = MeshReference;
// Application semantics supplement generic Scene field/schema validation.
Result<std::vector<MeshReference>> mesh_references(const scene::Document&);
Result<std::vector<MaterialReference>> material_references(const scene::Document&);
Result<std::optional<MaterialReference>> object_material(const scene::Object&);
struct ObjectRow {
  PersistentId id;
  scene::ObjectHandle handle;
  scene::LocalTransform local;
  scene::Affine world;
  scene::Vec3 dimensions{1,1,1}, color{.28,.52,.78};
  bool visible{};
  std::optional<MeshReference> mesh;
  PersistentId parent;
  std::optional<MaterialReference> material;
  bool lighting{}; // Carries the DirectionalLight/RenderOutput settings; never drawn.
};
// Rows, lighting settings, generation and origin from one consistent read snapshot.
struct SceneExtraction {
  std::vector<ObjectRow> rows;
  LightingSettings lighting;
};
// Compose one unpublished local transform and its descendants into owned rows.
// Move an owned extraction in to avoid another row copy; pass an lvalue to retain
// it. Source order, unrelated rows and all authored metadata remain unchanged.
// Failure discards only this owned candidate. Parents must resolve in these rows.
// Uses Scene affine composition, never an inverse or a World/history mutation.
Result<std::vector<ObjectRow>> preview_objects(std::vector<ObjectRow>,const ObjectRow& preview);
// Retained identity tokens cannot alias a later Document at a recycled address.
struct DocumentIdentity {};
struct PersistenceStamp {
  std::shared_ptr<const DocumentIdentity> document;
  std::uint64_t generation{}, play_epoch{};
};
class Document {
  struct ExtractionCache {
    std::uint64_t generation{};
    bool valid{};
    std::vector<ObjectRow> rows;
    LightingSettings lighting;
  };
  TrackedAllocator memory_;
  std::shared_ptr<const scene::Schemas> schemas_;
  std::unique_ptr<scene::World> world_, play_;
  std::uint64_t next_id_{1};
  double play_time_{};
  PersistentId play_target_;
  scene::Quat play_rotation_;
  std::shared_ptr<const DocumentIdentity> identity_{std::make_shared<const DocumentIdentity>()};
  std::uint64_t play_epoch_{};
  // The owned authoring cache is bounded by the World's object limit. Neither
  // ReadViews nor references into these rows escape objects(). Dynamic Play
  // extraction returns fresh rows without retaining another owned copy.
  mutable std::mutex extraction_mutex_;
  mutable ExtractionCache authoring_cache_;
  explicit Document(TrackedAllocator memory) : memory_(std::move(memory)) {}
  scene::World& world(bool play) const;
public:
  static Result<std::unique_ptr<Document>> create();
  ~Document();
  // Mutation/lifecycle methods remain World-owner-thread operations. Concurrent
  // extraction is supported while the selected World lifetime is stable; every
  // caller receives an independent owned vector, including on cache hits.
  Result<std::vector<ObjectRow>> objects(bool play=false) const;
  Result<SceneExtraction> extract(bool play=false) const;
  Result<LightingSettings> lighting(bool play=false) const;
  // One undoable transaction creates, updates or removes the settings object; an empty
  // request removes it. Values are validated before any command is published.
  Result<void> set_lighting(const LightingSettings&, std::uint64_t expected_generation);
  Result<void> reset();
  std::uint64_t generation(bool play=false) const;
  scene::LocalOrigin origin(bool play=false) const;
  Result<PersistentId> add_box();
  Result<PersistentId> add_mesh(const MeshReference&);
  Result<void> assign_material(PersistentId,std::optional<MaterialReference>);
  Result<void> edit(const ObjectRow&, std::uint64_t expected_generation);
  Result<void> remove(PersistentId, scene::ObjectHandle, std::uint64_t expected_generation);
  Result<void> undo();
  Result<void> redo();
  // Explicit owner action: preserve authored state while discarding undo/redo.
  Result<void> clear_history();
  // Owner-thread snapshot/admission. Worker codecs receive owned data and an
  // immutable schema, never a World or borrowed view.
  PersistenceStamp persistence_stamp() const;
  bool current(const PersistenceStamp&) const;
  Result<scene::Document> snapshot() const;
  std::shared_ptr<const scene::Schemas> schemas() const { return schemas_; }
  // The returned document owns its fields; borrowed bytes need only survive this call.
  static Result<scene::Document> decode(std::span<const std::byte>,const scene::Schemas&);
  static Result<scene::Document> decode(platform::Files&, std::string_view,
                                       const scene::Schemas&);
  static Result<void> write(platform::Files&, std::string_view,
                             const scene::Document&, const scene::Schemas&, bool cooked);
  Result<void> install(const scene::Document&, const PersistenceStamp&);
  // Editable source JSON and RACESCN1 runtime binary remain distinct.
  Result<void> save(platform::Files&, std::string_view path) const;
  Result<void> cook(platform::Files&, std::string_view path) const;
  Result<void> load(platform::Files&, std::string_view path);
  Result<void> start_play();
  Result<void> stop_play();
  Result<void> tick_play(double seconds);
  bool playing() const noexcept { return bool(play_); }
};
}
