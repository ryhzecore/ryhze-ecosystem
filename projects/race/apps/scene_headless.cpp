#include "race/build_identity.hpp"
#include "race/runtime/lifecycle.hpp"
#include "race/scene/scene.hpp"
#include <cstdio>
#include <string>
using namespace race;
using namespace race::scene;
using namespace std::chrono_literals;
namespace {
PersistentId id(std::uint64_t n) { return {73, n}; }
Object object(std::uint64_t n, Vec3 position = {}) {
  return {id(n), {}, {transform_component({position, {}, {1, 1, 1}})}, {}};
}
class SceneModule final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_{
      "scene-headless", 1, {}, 0, runtime::Memory | runtime::Jobs};
  platform::Files &files_;
  std::string path_;
  runtime::ModuleContext context_;
  std::shared_ptr<const Schemas> schemas_;
  std::shared_ptr<const Templates> templates_;
  std::unique_ptr<World> world_, isolated_;
  std::unique_ptr<Execution> execution_;
  Limits limits_;
  Result<void> workflow() {
    auto result = world_->apply(
        {{Command::create(object(1, {5, 0, 0})), Command::create(object(2, {0, 2, 0}))}});
    if (!result)
      return result;
    result = world_->apply({{Command::parent_to(id(2), {{}, id(1)}, ReparentMode::PreserveLocal),
                             Command::set(id(1), 1, 1, Vec3{7, 0, 0})}});
    if (!result)
      return result;
    result = world_->undo();
    if (!result)
      return result;
    {
      auto view = world_->read();
      if (!view)
        return view.error();
      auto h = view.value().find(id(1));
      auto child = view.value().find(id(2));
      if (!h || !child || std::get<Vec3>(view.value().value(h.value(), 1, 1).value()).x != 5 ||
          std::get<Reference>(view.value().value(child.value(), 1, 4).value()).object.valid())
        return fail(ErrorCode::Invariant, "grouped undo failed");
    }
    result = world_->redo();
    if (!result)
      return result;
    {
      auto held = world_->read();
      if (!held)
        return held.error();
      auto ticket = world_->enqueue({{Command::set(id(2), 1, 1, Vec3{0, 3, 0})}});
      if (!ticket)
        return ticket.error();
      auto blocked = world_->publish_pending();
      if (blocked || blocked.error().code != ErrorCode::Busy)
        return fail(ErrorCode::Invariant, "retained view did not block publication");
      auto child = held.value().find(id(2));
      if (!child || std::get<Vec3>(held.value().value(child.value(), 1, 1).value()).y != 2)
        return fail(ErrorCode::Invariant, "retained view changed");
      held.value().release();
      result = world_->publish_pending();
      if (!result)
        return result;
      auto completed = world_->take_completion(ticket.value());
      if (!completed)
        return completed.error();
      if (!completed.value().result)
        return completed.value().result;
    }
    std::array<PersistentId, 2> fresh{id(10), id(11)};
    result = world_->instantiate({id(500), 1}, id(600), fresh);
    if (!result)
      return result;
    result = world_->apply({{Command::set(id(11), 1, 1, Vec3{0, 9, 0})}});
    if (!result)
      return result;
    result = world_->update_instance(id(600), {id(500), 2});
    if (!result)
      return result;
    result = world_->declare_section({id(100), {}});
    if (!result)
      return result;
    auto prepare = world_->begin_prepare(id(100));
    if (!prepare)
      return prepare.error();
    auto section_object = object(20);
    section_object.section = id(100);
    result = world_->finish_prepare(prepare.value(), {section_object});
    if (!result)
      return result;
    result = world_->activate(id(100));
    if (!result)
      return result;
    auto snapshot = world_->document();
    if (!snapshot)
      return snapshot.error();
    result = save_json(files_, path_, *world_, *schemas_, limits_);
    if (!result)
      return result;
    auto bytes = files_.read(path_, limits_.document_bytes);
    if (!bytes)
      return bytes.error();
    std::string_view text(reinterpret_cast<const char *>(bytes.value().data()),
                          bytes.value().size());
    auto decoded = decode_json(text, *schemas_, limits_);
    if (!decoded)
      return decoded.error();
    result = isolated_->load(decoded.value());
    if (!result)
      return result;
    auto cooked = cook(snapshot.value(), *schemas_, limits_, build::authored_input_sha256);
    if (!cooked)
      return cooked.error();
    auto binary =
        decode_cooked(cooked.value().bytes, *schemas_, limits_, build::authored_input_sha256);
    if (!binary)
      return binary.error();
    auto original = encode_json(snapshot.value(), *schemas_, limits_);
    auto loaded = isolated_->document();
    if (!loaded)
      return loaded.error();
    auto loaded_json = encode_json(loaded.value(), *schemas_, limits_);
    auto one = encode_json(decoded.value(), *schemas_, limits_),
         two = encode_json(binary.value(), *schemas_, limits_);
    if (!original || !loaded_json || !one || !two || one.value() != original.value() ||
        two.value() != original.value() || loaded_json.value() != original.value())
      return fail(ErrorCode::Invariant, "authoring and cooked round trips disagree");
    {
      auto source = world_->read();
      if (!source)
        return source.error();
      auto target = isolated_->read();
      if (!target)
        return target.error();
      auto handle = source.value().find(id(1));
      if (!handle)
        return handle.error();
      auto foreign = target.value().object(handle.value());
      if (foreign || foreign.error().code != ErrorCode::StaleHandle)
        return fail(ErrorCode::Invariant, "world handles are not isolated");
    }
    System system{1,
                  {},
                  {{1, core::AccessMode::Write}},
                  [](const SystemContext &ctx, void *) -> Result<std::vector<Command>> {
                    auto h = ctx.view.find(id(1));
                    if (!h)
                      return h.error();
                    auto v = ctx.view.value(h.value(), 1, 1);
                    if (!v)
                      return v.error();
                    auto p = std::get<Vec3>(v.value());
                    p.x += 1;
                    return std::vector<Command>{Command::set(id(1), 1, 1, p, h.value())};
                  },
                  {}};
    auto step = core::Rational::make(1, 60).value();
    auto execution = Execution::create(*world_, *context_.jobs, {system}, step,
                                       {4, 8, core::OverloadAction::RejectElapsed});
    if (!execution)
      return execution.error();
    execution_ = std::move(execution).value();
    result = execution_->evaluate({EvaluationMode::ExactRational, {0, step}, {}},
                                  std::chrono::milliseconds::max());
    if (!result)
      return result;
    {
      auto view = world_->read();
      if (!view)
        return view.error();
      auto other = isolated_->read();
      if (!other)
        return other.error();
      auto h = view.value().find(id(1));
      auto oh = other.value().find(id(1));
      if (!h || !oh)
        return fail(ErrorCode::Invariant, "workflow object missing");
      if (std::get<Vec3>(view.value().value(h.value(), 1, 1).value()).x != 8 ||
          std::get<Vec3>(other.value().value(oh.value(), 1, 1).value()).x != 7)
        return fail(ErrorCode::Invariant, "isolated worlds shared mutable state");
    }
    result = world_->unload(id(100), 0ms);
    if (!result)
      return result;
    if (world_->resolve({id(100), id(20)}).state != ReferenceState::Unloaded)
      return fail(ErrorCode::Invariant, "unloaded reference status");
    return {};
  }

public:
  SceneModule(platform::Files &files, std::string path) : files_(files), path_(std::move(path)) {
    limits_.objects = 64;
    limits_.components = 128;
    limits_.template_objects = 64;
  }
  const runtime::ModuleDescriptor &descriptor() const noexcept override { return descriptor_; }
  Result<void> validate() noexcept override { return {}; }
  Result<void> initialize(runtime::ModuleContext context) noexcept override {
    context_ = context;
    try {
      auto s = Schemas::create();
      if (!s)
        return s.error();
      schemas_ = s.value();
      auto root = object(501);
      auto child = object(502, {0, 2, 0});
      child.components[0] = transform_component({{0, 2, 0}, {}, {1, 1, 1}}, {{}, id(501)});
      auto next = root;
      next.components[0] = transform_component({{1, 0, 0}, {}, {1, 1, 1}});
      auto t =
          Templates::create({{{id(500), 1}, {root, child}, {}}, {{id(500), 2}, {next, child}, {}}},
                            *schemas_, limits_);
      if (!t)
        return t.error();
      templates_ = t.value();
      auto w = World::create(*context.memory, schemas_, limits_, templates_);
      if (!w)
        return w.error();
      world_ = std::move(w).value();
      auto isolated = World::create(*context.memory, schemas_, limits_, templates_);
      if (!isolated)
        return isolated.error();
      isolated_ = std::move(isolated).value();
      return {};
    } catch (const std::bad_alloc &) {
      return fail(ErrorCode::Exhausted, "Scene initialization allocation");
    }
  }
  Result<void> start() noexcept override {
    try {
      return workflow();
    } catch (const std::bad_alloc &) {
      return fail(ErrorCode::Exhausted, "Scene workflow allocation");
    }
  }
  void request_stop() noexcept override {
    if (execution_) {
      auto cancel = execution_->cancel();
      (void)cancel;
    }
  }
  Result<void> quiesce(std::chrono::milliseconds timeout) noexcept override {
    if (execution_) {
      auto done = execution_->poll(timeout);
      if (!done && done.error().code != ErrorCode::NotFound &&
          done.error().code != ErrorCode::Cancelled)
        return done;
      execution_.reset();
    }
    if (world_) {
      auto r = world_->stop(timeout);
      if (!r)
        return r;
    }
    if (isolated_) {
      auto r = isolated_->stop(timeout);
      if (!r)
        return r;
    }
    return {};
  }
  void shutdown() noexcept override {
    execution_.reset();
    isolated_.reset();
    world_.reset();
    templates_.reset();
    schemas_.reset();
  }
  void rollback_initialization() noexcept override { shutdown(); }
};
} // namespace
int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--identity") {
    std::printf("{\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\",\"configuration\":\"%"
                "s\",\"sanitizer\":\"%s\"}\n",
                build::source_commit, build::authored_input_sha256, build::configuration,
                build::sanitizer);
    return 0;
  }
  if (argc != 3 || std::string_view(argv[1]) != "--output") {
    std::fputs("usage: race_scene_headless --output ABSOLUTE_SCENE_JSON\n", stderr);
    return 2;
  }
  auto services = platform::make_services();
  if (!services)
    return 1;
  auto allocator = TrackedAllocator::create({16 * 1024 * 1024});
  if (!allocator)
    return 1;
  auto scheduler = core::Scheduler::create(*services.value().threads, {32, 2, 8});
  if (!scheduler)
    return 1;
  core::Diagnostics diagnostics(*services.value().clock);
  bool success = false;
  {
    runtime::Runtime runtime(allocator.value(), *scheduler.value(), *services.value().clock,
                             diagnostics);
    auto added = runtime.add(std::make_unique<SceneModule>(*services.value().files, argv[2]));
    if (!added)
      return 1;
    auto started = runtime.start();
    if (!started)
      std::fprintf(stderr, "scene workflow: %s\n", started.error().message);
    auto stopped = runtime.stop(std::chrono::milliseconds::max());
    success = bool(started) && bool(stopped);
  }
  auto stopped =
      scheduler.value()->shutdown(core::ShutdownMode::Drain, std::chrono::milliseconds::max());
  if (!stopped || allocator.value().snapshot().total.live || scheduler.value()->stats().occupied)
    success = false;
  std::printf("{\"scene_headless_passed\":%s,\"source_commit\":\"%s\",\"authored_input_sha256\":\"%"
              "s\",\"tracked_live_bytes_after_stop\":%zu}\n",
              success ? "true" : "false", build::source_commit, build::authored_input_sha256,
              allocator.value().snapshot().total.live);
  return success ? 0 : 1;
}
