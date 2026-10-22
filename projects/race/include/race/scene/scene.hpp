#pragma once
#include "race/core/jobs.hpp"
#include "race/core/memory.hpp"
#include "race/core/time.hpp"
#include "race/foundation/identity.hpp"
#include "race/scene/math.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace race::scene {
using TypeId = std::uint64_t;
using FieldId = std::uint64_t;
inline constexpr TypeId transform_type = 1;
inline constexpr FieldId translation_field = 1, rotation_field = 2, scale_field = 3,
                         parent_field = 4;
inline constexpr std::size_t max_types = 16, max_fields = 8, max_template_depth = 8;
struct Reference {
  PersistentId section, object;
  friend bool operator==(const Reference &, const Reference &) = default;
};
using Value = std::variant<std::int64_t, double, bool, Vec3, Quat, Reference, PersistentId>;
enum class ValueKind { Integer, Scalar, Boolean, Vector, Quaternion, ObjectReference, AssetId };
struct FieldSchema {
  FieldId id{};
  std::string name;
  Value default_value{std::int64_t{0}};
  double minimum{-1e300}, maximum{1e300};
  bool exposed{false};
};
struct ComponentSchema {
  TypeId id{};
  std::uint32_t version{1};
  std::string name;
  std::vector<FieldSchema> fields;
};
class Schemas {
  std::vector<ComponentSchema> types_;
  explicit Schemas(std::vector<ComponentSchema> t) : types_(std::move(t)) {}

public:
  static Result<std::shared_ptr<const Schemas>>
  create(std::vector<ComponentSchema> additional = {});
  std::span<const ComponentSchema> types() const noexcept { return types_; }
  const ComponentSchema *find(TypeId) const noexcept;
  Result<std::size_t> index(TypeId) const noexcept;
  Result<std::size_t> field_index(TypeId, FieldId) const noexcept;
  Result<void> validate(TypeId, FieldId, const Value &) const noexcept;
};
struct FieldValue {
  FieldId id{};
  Value value{std::int64_t{0}};
  friend bool operator==(const FieldValue &, const FieldValue &) = default;
};
struct Component {
  TypeId type{};
  std::uint32_t version{1};
  std::vector<FieldValue> fields;
  friend bool operator==(const Component &, const Component &) = default;
};
Component transform_component(LocalTransform = {}, Reference parent = {});
struct TemplateKey {
  PersistentId id;
  std::uint64_t revision{};
  friend bool operator==(const TemplateKey &, const TemplateKey &) = default;
};
struct TemplatePath {
  std::array<PersistentId, max_template_depth + 1> nodes{};
  std::uint8_t length{};
  friend bool operator==(const TemplatePath &, const TemplatePath &) = default;
};
struct Override {
  TypeId type{};
  FieldId field{};
  friend bool operator==(const Override &, const Override &) = default;
};
struct Binding {
  PersistentId instance;
  TemplateKey pinned;
  TemplatePath path;
  std::vector<Override> overrides;
  friend bool operator==(const Binding &, const Binding &) = default;
};
struct Object {
  PersistentId id, section;
  std::vector<Component> components;
  std::optional<Binding> binding;
  friend bool operator==(const Object &, const Object &) = default;
};
struct ObjectIdentity {
  PersistentId id;
};
using ObjectHandle = Handle<ObjectIdentity>;
struct ComponentRow {
  ObjectHandle object;
  std::array<Value, max_fields> values{};
};
enum class ReparentMode { PreserveLocal, PreserveWorld };
enum class CommandKind {
  Create,
  Destroy,
  PutComponent,
  RemoveComponent,
  SetField,
  Reparent,
  Replace
};
struct Command {
  CommandKind kind{CommandKind::Create};
  PersistentId target;
  std::optional<ObjectHandle> expected_handle;
  Object object;
  Component component;
  TypeId type{};
  FieldId field{};
  Value value{std::int64_t{0}};
  Reference parent;
  ReparentMode reparent{ReparentMode::PreserveLocal};
  static Command create(Object);
  static Command destroy(PersistentId, std::optional<ObjectHandle> = {});
  static Command set(PersistentId, TypeId, FieldId, Value, std::optional<ObjectHandle> = {});
  static Command parent_to(PersistentId, Reference, ReparentMode);
};
enum class EditMode { Authoring, Simulation };
struct Transaction {
  std::vector<Command> commands;
  EditMode mode{EditMode::Authoring};
};
struct CommandCompletion {
  std::uint64_t ticket{};
  Result<void> result;
};
struct Limits {
  std::size_t objects{1024}, components{4096}, commands_per_transaction{4096};
  std::size_t transaction_bytes{8 * 1024 * 1024}, pending_transactions{16},
      pending_bytes{16 * 1024 * 1024};
  std::size_t history_groups{32}, history_bytes{32 * 1024 * 1024}, views{16};
  std::size_t sections{32}, prepared_bytes{16 * 1024 * 1024}, document_bytes{64 * 1024 * 1024},
      json_scratch_bytes{256 * 1024 * 1024};
  std::size_t hierarchy_depth{256}, template_depth{max_template_depth}, template_objects{1024},
      template_revisions{64};
};
struct WorldStats {
  std::size_t objects{}, components{}, readers{}, retained_generations{}, pending_transactions{},
      pending_bytes{}, history_groups{}, history_bytes{}, prepared_bytes{};
  std::size_t tracked_pool_bytes{}, bounded_metadata_capacity_bytes{};
  std::uint64_t generation{}, derived_recomputations{}, transaction_touched_objects{},
      publications{};
};
struct WorldState;
// A move-only const borrow. Spans/values returned by it must not outlive it.
class ReadView {
  std::shared_ptr<WorldState> state_;
  explicit ReadView(std::shared_ptr<WorldState>);
  friend class World;
  friend class Execution;

public:
  ReadView() = default;
  ~ReadView();
  ReadView(ReadView &&) noexcept;
  ReadView &operator=(ReadView &&) noexcept;
  ReadView(const ReadView &) = delete;
  ReadView &operator=(const ReadView &) = delete;
  void release() noexcept;
  std::uint64_t generation() const noexcept;
  Result<ObjectHandle> find(PersistentId) const;
  Result<Object> object(ObjectHandle) const;
  Result<Value> value(ObjectHandle, TypeId, FieldId) const;
  Result<Affine> world_transform(ObjectHandle) const;
  Result<std::span<const ComponentRow>> query(TypeId) const;
  std::span<const ObjectHandle> objects() const noexcept;
  LocalOrigin origin() const noexcept;
};
enum class SectionState {
  Declared,
  Preparing,
  Prepared,
  Active,
  Unloading,
  Unloaded,
  Failed,
  Cancelled
};
struct SectionDefinition {
  PersistentId id;
  std::vector<PersistentId> required;
  friend bool operator==(const SectionDefinition &, const SectionDefinition &) = default;
};
struct SectionInfo {
  SectionDefinition definition;
  SectionState state{SectionState::Declared};
  std::vector<PersistentId> manifest;
  std::optional<ErrorCode> last_failure;
};
struct Preparation {
  PersistentId section;
  std::uint64_t ticket{}, world{};
};
enum class ReferenceState { Available, Unloaded, Missing, Invalid };
struct ResolvedReference {
  ReferenceState state;
  std::optional<ObjectHandle> object;
};
struct NestedTemplate {
  PersistentId anchor;
  TemplateKey dependency;
};
struct TemplateRevision {
  TemplateKey key;
  std::vector<Object> objects;
  std::vector<NestedTemplate> nested;
};
class Templates {
  std::vector<TemplateRevision> revisions_;
  explicit Templates(std::vector<TemplateRevision> r) : revisions_(std::move(r)) {}

public:
  static Result<std::shared_ptr<const Templates>> create(std::vector<TemplateRevision>,
                                                         const Schemas &, Limits);
  const TemplateRevision *find(TemplateKey) const noexcept;
  std::span<const TemplateRevision> revisions() const noexcept { return revisions_; }
};
struct Document {
  std::uint32_t schema{1};
  LocalOrigin origin;
  std::vector<Object> objects;
  std::vector<SectionInfo> sections;
};
class World {
  std::shared_ptr<WorldState> state_;
  friend class Execution;
  explicit World(std::shared_ptr<WorldState>);

public:
  static Result<std::unique_ptr<World>> create(TrackedAllocator &, std::shared_ptr<const Schemas>,
                                               Limits = {}, std::shared_ptr<const Templates> = {});
  ~World();
  World(const World &) = delete;
  World &operator=(const World &) = delete;
  Result<ReadView> read() const;
  Result<void> apply(const Transaction &);
  Result<std::uint64_t> enqueue(Transaction); // MPMC admission owns complete inputs.
  Result<void> publish_pending();             // Owner thread, never waits on a held reader.
  Result<CommandCompletion> take_completion(std::uint64_t);
  Result<void> undo();
  Result<void> redo();
  Result<void> clear_history();
  Result<void> set_origin(Vec3);
  WorldStats stats() const;
  Result<void> declare_section(SectionDefinition);
  Result<Preparation> begin_prepare(PersistentId);
  Result<void> cancel_prepare(Preparation); // Thread safe; ticket checked.
  Result<void> finish_prepare(Preparation, std::vector<Object>);
  Result<void> activate(PersistentId);
  Result<void> unload(PersistentId, std::chrono::milliseconds);
  Result<SectionInfo> section(PersistentId) const;
  ResolvedReference resolve(Reference) const;
  Result<void> instantiate(TemplateKey, PersistentId instance,
                           std::span<const PersistentId> fresh_ids, PersistentId section = {});
  Result<void> update_instance(PersistentId, TemplateKey,
                               std::span<const PersistentId> added_ids = {});
  Result<void> reset_override(PersistentId object, TypeId, FieldId);
  Result<void> detach(PersistentId instance);
  Result<Document> document() const;
  Result<void> load(const Document &);
  // Nominal quiescence-wait-stage timeout, not an API-entry deadline.
  // Timeout closes new consumers and retains every live owner.
  Result<void> stop(std::chrono::milliseconds);
  std::vector<PersistentId> teardown_order() const;
};

Result<std::string> encode_json(const Document &, const Schemas &, Limits);
Result<Document> decode_json(std::string_view, const Schemas &, Limits);
struct Cooked {
  std::vector<std::byte> bytes;
};
Result<Cooked> cook(const Document &, const Schemas &, Limits, std::string_view source_identity);
Result<Document> decode_cooked(std::span<const std::byte>, const Schemas &, Limits,
                               std::string_view expected_source = {});
Result<void> save_json(platform::Files &, std::string_view, const World &, const Schemas &, Limits,
                       platform::Durability = platform::Durability::AtomicVisibility);

enum class EvaluationMode { FixedTick, ExactRational };
struct Evaluation {
  EvaluationMode mode{EvaluationMode::FixedTick};
  core::SimulationTime tick;
  core::Rational exact_time;
};
struct SystemContext {
  const ReadView &view;
  const Evaluation &evaluation;
  const core::JobContext &job;
};
using SystemFunction = Result<std::vector<Command>> (*)(const SystemContext &, void *);
struct System {
  std::uint64_t id{};
  std::vector<std::uint64_t> predecessors;
  std::vector<core::ResourceAccess> access;
  SystemFunction function{};
  std::shared_ptr<void> context;
};
class Execution {
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Execution(std::unique_ptr<Impl>);

public:
  static Result<std::unique_ptr<Execution>> create(World &, core::Scheduler &, std::vector<System>,
                                                   core::Rational step, core::RealtimePolicy);
  ~Execution();
  Result<void> evaluate(Evaluation, std::chrono::milliseconds);
  Result<void> poll(std::chrono::milliseconds);
  Result<void> cancel();
  Result<core::StepReport> advance(platform::MonotonicTime, std::chrono::milliseconds);
  Result<void> resume(platform::MonotonicTime);
  std::uint64_t published_tick() const noexcept;
};
} // namespace race::scene
