#include "race/audio/audio.hpp"
#include "race/build_identity.hpp"
#include "race/runtime/lifecycle.hpp"
#include "race/scene/scene.hpp"
#include "race/vfx/vfx.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace race;
namespace {
constexpr std::uint64_t ticks = 120, rebase_tick = 61;
constexpr std::size_t frames_per_tick = audio::output_rate / 60, particle_limit = 16;
constexpr PersistentId world_id{20261010, 1}, session_id{20261010, 2}, source_id{20261010, 3};
constexpr double particle_tolerance = 1e-10; // Absolute metres, this executable/configuration.
struct SourceSnapshot {
  PersistentId world, session, source;
  std::uint64_t tick{}, generation{};
  scene::ObjectHandle handle;
  scene::Affine transform;
  scene::LocalOrigin origin;
};
struct ParticleSample {
  std::uint64_t tick{};
  vfx::Particle particle; // Owned copy; position converted to absolute metres.
};
struct RunData {
  std::vector<std::int16_t> pcm;
  std::vector<ParticleSample> particles;
  std::weak_ptr<const audio::Clip> clip_observer;
  audio::Statistics audio_stats;
  vfx::Statistics vfx_stats;
  std::uint64_t publications{}, generation{}, origin_revision{}, released_reads{};
  std::size_t peak_particles{}, tracked_live_after_stop{}, scheduler_occupied{};
  unsigned initialized{}, started{}, stop_requested{}, quiesced{}, shut_down{};
  bool runtime_stopped{}, lease_released{};
};

// This executable is the composition owner. All Scene/Audio/VFX access stays on
// the Runtime owner thread; no borrowed Scene memory crosses into either domain.
class EffectsModule final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_{"effects-headless", 1, {}, 0, runtime::Memory};
  RunData& output_;
  bool rebase_, accepting_{};
  std::shared_ptr<const scene::Schemas> schemas_;
  std::unique_ptr<scene::World> world_;
  std::unique_ptr<vfx::ParticleSystem> particles_;
  audio::Mixer mixer_;
  audio::ClipLease clip_;
  audio::VoiceHandle voice_;
  bool voice_live_{};
  vfx::EmitterHandle emitter_;
  scene::ObjectHandle source_;
  std::uint64_t previous_generation_{}, serial_{};

  Result<SourceSnapshot> copy_source(std::uint64_t tick) {
    auto read = world_->read();
    if (!read) return read.error();
    auto handle = read.value().find(source_id);
    if (!handle) return handle.error();
    auto transform = read.value().world_transform(handle.value());
    if (!transform) return transform.error();
    SourceSnapshot owned{world_id, session_id, source_id, tick, read.value().generation(),
                         handle.value(), transform.value(), read.value().origin()};
    read.value().release();
    if (world_->stats().readers != 0)
      return fail(ErrorCode::Invariant, "Scene borrow survived extraction");
    return owned;
  }
public:
  EffectsModule(RunData& output, bool rebase) : output_(output), rebase_(rebase) {}
  const runtime::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
  Result<void> validate() noexcept override { return {}; }
  Result<void> initialize(runtime::ModuleContext context) noexcept override {
    try {
      output_.pcm.reserve(ticks * frames_per_tick * 2);
      output_.particles.reserve(ticks * particle_limit);
      auto schemas = scene::Schemas::create();
      if (!schemas) return schemas.error();
      schemas_ = schemas.value();
      scene::Limits limits;
      limits.objects = 2; limits.components = 2; limits.commands_per_transaction = 1;
      limits.transaction_bytes = 4096; limits.pending_transactions = 1;
      limits.pending_bytes = 4096; limits.history_groups = 1; limits.history_bytes = 4096;
      limits.views = 1; limits.sections = 1; limits.template_objects = 2;
      auto world = scene::World::create(*context.memory, schemas_, limits);
      if (!world) return world.error();
      world_ = std::move(world).value();
      scene::Object source{source_id, {}, {scene::transform_component()}, {}};
      auto created = world_->apply({{scene::Command::create(std::move(source))}, scene::EditMode::Simulation});
      if (!created) return created;
      auto initial = copy_source(0);
      if (!initial) return initial.error();
      source_ = initial.value().handle; previous_generation_ = initial.value().generation;
      vfx::Config config;
      config.world = world_id; config.max_emitters = 1; config.max_particles = particle_limit;
      config.max_events = 2; config.max_event_particles = 2; config.max_spawn_attempts_per_tick = 3;
      auto system = vfx::ParticleSystem::create(config);
      if (!system) return system.error();
      particles_ = std::move(system).value();
      vfx::EmitterDesc emitter;
      emitter.source = source_id; emitter.seed = 731; emitter.lifetime_ticks = 12;
      emitter.velocity = {0, 2, 0}; emitter.velocity_spread = {.25, .5, .25};
      emitter.acceleration = {0, -1, 0};
      auto admitted = particles_->create_emitter(emitter);
      if (!admitted) return admitted.error();
      emitter_ = admitted.value();
      // A bounded, integer-authored waveform avoids a platform sine dependency.
      std::array<std::int16_t, 48> waveform{};
      for (std::size_t i = 0; i < waveform.size(); ++i)
        waveform[i] = static_cast<std::int16_t>((i < 24 ? static_cast<int>(i) : 48 - static_cast<int>(i)) * 500 - 6000);
      auto clip = audio::make_pcm16_clip(audio::output_rate, 1, waveform);
      if (!clip) return clip.error();
      clip_ = clip.value(); output_.clip_observer = clip_;
      auto limited = mixer_.set_limits({1, clip_->bytes()});
      if (!limited) return limited;
      auto bus = mixer_.set_bus_gain(1, .75);
      if (!bus) return bus;
      ++output_.initialized;
      return {};
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "effects initialization allocation"); }
  }
  Result<void> start() noexcept override { accepting_ = true; ++output_.started; return {}; }
  Result<void> run() {
    if (!accepting_) return fail(ErrorCode::Stopping, "effects not running");
    const auto step = core::Rational::make(1, 60);
    if (!step) return step.error();
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
      const core::SimulationTime time{tick, step.value()};
      // Authored motion, not an invented physics phase. Binary-exact coordinates
      // isolate origin continuity from trigonometric/input rounding differences.
      const scene::Vec3 absolute{-1 + static_cast<double>(time.tick_index) / 64, 1, 0};
      auto published = world_->apply({{scene::Command::set(source_id, scene::transform_type,
          scene::translation_field, absolute, source_)}, scene::EditMode::Simulation});
      if (!published) return published;
      if (rebase_ && tick == rebase_tick) {
        // Owner boundary: no consumers run between these validated fixture changes.
        // A participant failure aborts this run before any consumer publication.
        auto origin = world_->set_origin({1024, 64, -256});
        if (!origin) return origin;
      }
      auto copied = copy_source(tick);
      if (!copied) return copied.error();
      const auto& input = copied.value();
      const auto expected_generation = previous_generation_ + 1 + (rebase_ && tick == rebase_tick ? 1 : 0);
      if (input.world != world_id || input.session != session_id || input.source != source_id ||
          input.tick != tick || input.handle != source_ || input.generation != expected_generation)
        return fail(ErrorCode::Invariant, "owned Scene provenance mismatch");
      previous_generation_ = input.generation; ++output_.released_reads;
      const auto& m = input.transform.m;
      const scene::Vec3 position{m[3], m[7], m[11]};
      if (position != absolute) return fail(ErrorCode::Invariant, "authoritative motion lost");
      const auto origin = input.origin;
      if (origin.revision != particles_->origin_revision()) {
        auto a = mixer_.rebase({origin.position.x, origin.position.y, origin.position.z}, origin.revision);
        if (!a) return a;
        auto v = particles_->rebase({origin.position.x, origin.position.y, origin.position.z}, origin.revision);
        if (!v) return v;
      }
      auto relative = scene::relative_position(position, origin);
      if (!relative) return relative.error();
      const auto p = relative.value().metres;
      audio::Play sound;
      sound.source = input.source; sound.start_frame = (input.tick - 1) * frames_per_tick;
      sound.loop = true; sound.spatial = true; sound.bus = 1; sound.gain = .5;
      sound.position = {p.x, p.y, p.z}; sound.origin_revision = origin.revision;
      sound.minimum_distance = 1; sound.maximum_distance = 8;
      if (!voice_live_) {
        auto played = mixer_.play(clip_, sound);
        if (!played) return played.error();
        voice_ = played.value(); voice_live_ = true;
      } else {
        auto moved = mixer_.set_position(voice_, sound.position, origin.revision);
        if (!moved) return moved;
      }
      // Intentional cosmetic capacity pressure must remain observable and cannot
      // roll back the already published Scene generation.
      auto excess_voice = mixer_.play(clip_, sound);
      if (excess_voice || excess_voice.error().code != ErrorCode::QueueFull)
        return fail(ErrorCode::Invariant, "audio admission did not report exhaustion");
      for (unsigned event_index = 0; event_index < 3; ++event_index) {
        vfx::Event event;
        event.emitter = emitter_; event.world = input.world; event.source = input.source;
        event.tick = input.tick; event.serial = ++serial_; event.origin_revision = origin.revision;
        event.position = {p.x, p.y, p.z}; event.particles = 2;
        event.payload = vfx::GameplayBurst{{0, .25, 0}};
        auto sent = particles_->submit(event);
        if (!sent) return sent.error();
        if ((event_index == 2) != (sent.value().admission == vfx::Admission::Dropped))
          return fail(ErrorCode::Invariant, "VFX overload policy mismatch");
      }
      auto advanced = particles_->step(input.tick);
      if (!advanced) return advanced.error();
      std::array<std::int16_t, frames_per_tick * 2> block{};
      auto mixed = mixer_.mix(block);
      if (!mixed) return mixed.error();
      output_.pcm.insert(output_.pcm.end(), block.begin(), block.end());
      const auto live = particles_->particles();
      output_.peak_particles = std::max(output_.peak_particles, live.size());
      for (const auto& particle : live) {
        if (particle.source != input.source || particle.origin_revision != origin.revision)
          return fail(ErrorCode::Invariant, "particle provenance mismatch");
        auto owned = particle;
        owned.position.x += origin.position.x;
        owned.position.y += origin.position.y;
        owned.position.z += origin.position.z;
        output_.particles.push_back({input.tick, owned});
      }
      if (world_->stats().generation != input.generation || world_->stats().readers != 0)
        return fail(ErrorCode::Invariant, "cosmetic work altered Scene publication");
      output_.generation = input.generation; output_.origin_revision = origin.revision;
    }
    output_.publications = world_->stats().publications;
    output_.vfx_stats = particles_->statistics();
    return {};
  }
  void request_stop() noexcept override { accepting_ = false; ++output_.stop_requested; }
  Result<void> quiesce(std::chrono::milliseconds timeout) noexcept override {
    if (voice_live_) {
      auto stopped = mixer_.stop(voice_, mixer_.frame());
      if (!stopped) return stopped;
      std::array<std::int16_t, 2> retirement{};
      auto mixed = mixer_.mix(retirement); // No playback frame enters the delivered PCM.
      if (!mixed) return mixed.error();
      if (retirement[0] || retirement[1] || mixer_.collect_finished() != 1)
        return fail(ErrorCode::Invariant, "audio retirement failed");
      voice_live_ = false;
    }
    output_.audio_stats = mixer_.statistics();
    if (world_) {
      auto stopped = world_->stop(timeout);
      if (!stopped) return stopped;
    }
    ++output_.quiesced;
    return {};
  }
  void shutdown() noexcept override {
    particles_.reset(); clip_.reset(); world_.reset(); schemas_.reset(); ++output_.shut_down;
  }
  void rollback_initialization() noexcept override { shutdown(); }
};

Result<void> execute(TrackedAllocator& memory, core::Scheduler& jobs, platform::Clock& clock,
                     core::Diagnostics& diagnostics, RunData& data, bool rebase) {
  Result<void> outcome;
  {
    runtime::Runtime runtime(memory, jobs, clock, diagnostics);
    auto module = std::make_unique<EffectsModule>(data, rebase);
    auto* owner = module.get();
    auto added = runtime.add(std::move(module));
    if (!added) return added;
    outcome = runtime.start();
    if (outcome) outcome = owner->run();
    auto stopped = runtime.stop(std::chrono::milliseconds::max());
    if (!stopped) return stopped;
    auto state = runtime.state("effects-headless");
    data.runtime_stopped = state && state.value() == runtime::ModuleState::Stopped && !runtime.running();
  }
  data.tracked_live_after_stop = memory.snapshot().total.live;
  data.scheduler_occupied = jobs.stats().occupied;
  data.lease_released = data.clip_observer.expired();
  return outcome;
}
void little(std::ostream& out, std::uint32_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (i * 8)) & 255));
}
bool write_wave(const std::filesystem::path& path, const RunData& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const auto bytes = static_cast<std::uint32_t>(data.pcm.size() * 2);
  out.write("RIFF", 4); little(out, 36 + bytes, 4); out.write("WAVEfmt ", 8); little(out, 16, 4);
  little(out, 1, 2); little(out, 2, 2); little(out, audio::output_rate, 4);
  little(out, audio::output_rate * 4, 4); little(out, 4, 2); little(out, 16, 2);
  out.write("data", 4); little(out, bytes, 4);
  for (auto sample : data.pcm) little(out, static_cast<std::uint16_t>(sample), 2);
  out.close(); return static_cast<bool>(out);
}
bool clean(const RunData& data) {
  return data.initialized == 1 && data.started == 1 && data.stop_requested == 1 &&
         data.quiesced == 1 && data.shut_down == 1 && data.runtime_stopped && data.lease_released &&
         !data.tracked_live_after_stop && !data.scheduler_occupied && !data.audio_stats.resident_voices &&
         !data.audio_stats.retained_bytes && data.audio_stats.completed == 1;
}
void report(const char* name, const RunData& data) {
  std::printf("{\"run\":\"%s\",\"ticks\":%llu,\"tick_rate\":60,\"pcm_frames\":%zu,"
      "\"scene_publications\":%llu,\"scene_generation\":%llu,\"origin_revision\":%llu,"
      "\"scene_reads_released\":%llu,\"particle_samples\":%zu,\"peak_particles\":%zu,"
      "\"particles_spawned\":%llu,\"particles_expired\":%llu,\"particles_dropped\":%llu,"
      "\"events_dropped\":%llu,\"rebases\":%llu,\"audio_rejected\":%llu,"
      "\"audio_completed\":%llu,\"audio_clipped_samples\":%llu,\"lifecycle_clean\":%s,"
      "\"tracked_live_bytes_after_stop\":%zu}\n", name, static_cast<unsigned long long>(ticks),
      data.pcm.size() / 2, static_cast<unsigned long long>(data.publications),
      static_cast<unsigned long long>(data.generation), static_cast<unsigned long long>(data.origin_revision),
      static_cast<unsigned long long>(data.released_reads), data.particles.size(), data.peak_particles,
      static_cast<unsigned long long>(data.vfx_stats.spawned_particles),
      static_cast<unsigned long long>(data.vfx_stats.expired_particles),
      static_cast<unsigned long long>(data.vfx_stats.dropped_particles),
      static_cast<unsigned long long>(data.vfx_stats.dropped_events),
      static_cast<unsigned long long>(data.vfx_stats.rebases),
      static_cast<unsigned long long>(data.audio_stats.rejected),
      static_cast<unsigned long long>(data.audio_stats.completed),
      static_cast<unsigned long long>(data.audio_stats.clipped_samples), clean(data) ? "true" : "false",
      data.tracked_live_after_stop);
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--identity") {
    std::printf("{\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\"}\n",
                build::source_commit, build::authored_input_sha256);
    return 0;
  }
  if (argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--output-dir" || !argv[2][0])) {
    std::fputs("usage: race_effects_headless [--output-dir DIRECTORY] | --identity\n", stderr); return 2;
  }
  try {
    auto services = platform::make_services(); if (!services) return 3;
    auto memory = TrackedAllocator::create({2 * 1024 * 1024}); if (!memory) return 3;
    auto jobs = core::Scheduler::create(*services.value().threads, {16, 1, 4}); if (!jobs) return 3;
    core::Diagnostics diagnostics(*services.value().clock);
    RunData baseline, rebased;
    auto a = execute(memory.value(), *jobs.value(), *services.value().clock, diagnostics, baseline, false);
    auto b = execute(memory.value(), *jobs.value(), *services.value().clock, diagnostics, rebased, true);
    auto stopped = jobs.value()->shutdown(core::ShutdownMode::Drain, std::chrono::milliseconds::max());
    if (!a || !b || !stopped) {
      std::fprintf(stderr, "effects workflow failed: %s\n", !a ? a.error().message : !b ? b.error().message : stopped.error().message);
      return 4;
    }
    bool identities_equal = baseline.particles.size() == rebased.particles.size();
    double max_error = 0;
    if (identities_equal) for (std::size_t i = 0; i < baseline.particles.size(); ++i) {
      const auto& a_sample = baseline.particles[i]; const auto& b_sample = rebased.particles[i];
      const auto& p = a_sample.particle; const auto& q = b_sample.particle;
      identities_equal = identities_equal && a_sample.tick == b_sample.tick && p.id == q.id &&
          p.source == q.source && p.birth_tick == q.birth_tick && p.age_ticks == q.age_ticks &&
          p.lifetime_ticks == q.lifetime_ticks && p.effect_revision == q.effect_revision &&
          p.velocity == q.velocity && p.radius == q.radius && p.color == q.color &&
          std::isfinite(p.position.x) && std::isfinite(p.position.y) && std::isfinite(p.position.z) &&
          std::isfinite(q.position.x) && std::isfinite(q.position.y) && std::isfinite(q.position.z);
      max_error = std::max({max_error, std::abs(p.position.x - q.position.x),
                           std::abs(p.position.y - q.position.y), std::abs(p.position.z - q.position.z)});
    }
    const bool pcm_equal = baseline.pcm == rebased.pcm;
    const bool passed = clean(baseline) && clean(rebased) && identities_equal && pcm_equal &&
        max_error <= particle_tolerance && baseline.released_reads == ticks && rebased.released_reads == ticks &&
        baseline.audio_stats.rejected == ticks && rebased.audio_stats.rejected == ticks &&
        baseline.vfx_stats.dropped_events == ticks && rebased.vfx_stats.dropped_events == ticks &&
        baseline.vfx_stats.dropped_particles > 0 && baseline.publications == ticks + 1 &&
        rebased.publications == baseline.publications && rebased.generation == baseline.generation + 1 &&
        baseline.peak_particles <= particle_limit && rebased.peak_particles <= particle_limit;
    if (argc == 3) {
      const std::filesystem::path output(argv[2]); std::filesystem::create_directories(output);
      if (!write_wave(output / "baseline.wav", baseline) || !write_wave(output / "rebased.wav", rebased)) return 5;
    }
    report("baseline", baseline); report("rebased", rebased);
    std::printf("{\"effects_headless_passed\":%s,\"pcm_exact\":%s,\"particle_identity_exact\":%s,"
        "\"particle_max_error_metres\":%.17g,\"particle_tolerance_metres\":%.17g,"
        "\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\","
        "\"tracked_peak_bytes\":%zu,\"tracked_live_bytes_after_stop\":%zu,"
        "\"offline_owner_thread\":true,\"device_playback\":false,\"gpu_rendering\":false}\n",
        passed ? "true" : "false", pcm_equal ? "true" : "false", identities_equal ? "true" : "false",
        max_error, particle_tolerance, build::source_commit, build::authored_input_sha256,
        memory.value().snapshot().total.peak, memory.value().snapshot().total.live);
    return passed ? 0 : 6;
  } catch (const std::exception& error) { std::fprintf(stderr, "effects workflow: %s\n", error.what()); return 7; }
}
