#pragma once
#include "race/foundation/identity.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace race::audio {
inline constexpr std::uint32_t output_rate = 48000;
inline constexpr std::size_t max_voices = 16, max_buses = 4, max_block_frames = 4096;
struct DecodeLimits { std::size_t input_bytes{8 * 1024 * 1024}; std::size_t frames{1920000}; };
class Clip {
  std::uint32_t rate_{};
  std::uint16_t channels_{};
  std::vector<std::int16_t> samples_;
  Clip(std::uint32_t, std::uint16_t, std::span<const std::int16_t>);
  friend Result<std::shared_ptr<const Clip>> make_pcm16_clip(
      std::uint32_t, std::uint16_t, std::span<const std::int16_t>, DecodeLimits);
public:
  std::uint32_t sample_rate() const noexcept { return rate_; }
  std::uint16_t channels() const noexcept { return channels_; }
  std::size_t frames() const noexcept { return samples_.size() / channels_; }
  std::size_t bytes() const noexcept { return samples_.size() * sizeof(std::int16_t); }
  std::span<const std::int16_t> samples() const noexcept { return samples_; }
};
using ClipLease = std::shared_ptr<const Clip>;
Result<ClipLease> make_pcm16_clip(std::uint32_t sample_rate, std::uint16_t channels,
    std::span<const std::int16_t> samples, DecodeLimits limits = {});
// Strict RIFF/WAVE PCM16 LE, mono/stereo, 8..192 kHz. Unknown RIFF chunks are
// optional opaque chunks; their bounded extents/padding are still validated.
Result<ClipLease> decode_pcm16_wave(std::span<const std::byte>, DecodeLimits = {});
struct Vec3 { double x{}, y{}, z{}; };
struct Listener { Vec3 position{}; Vec3 right{1,0,0}; std::uint64_t origin_revision{1}; };
enum class Admission { Reject, StealLowerPriority };
struct Play {
  PersistentId source{1,1}, material{};
  std::uint64_t start_frame{};
  double gain{1};
  std::uint8_t bus{}, priority{};
  bool loop{}, spatial{};
  Vec3 position{}; // metres relative to the current origin, right-handed
  std::uint64_t origin_revision{1};
  double minimum_distance{1}, maximum_distance{100};
  Admission admission{Admission::Reject};
};
struct VoiceHandle {
  std::uint64_t owner{}, generation{};
  std::uint32_t slot{};
  friend bool operator==(VoiceHandle, VoiceHandle) = default;
};
struct Limits { std::size_t voices{max_voices}, retained_bytes{16 * 1024 * 1024}; };
struct Statistics {
  std::uint64_t admitted{}, rejected{}, stolen{}, completed{}, clipped_samples{};
  std::size_t resident_voices{}, retained_bytes{};
};
struct MixReport {
  std::size_t frames{}, started{}, finished{}, active{};
  std::uint64_t clipped_samples{};
};
// ONE serialized owner for all methods, including reads and destruction. No
// concurrent callback/control access is supported. Copy immutable Clips between
// owners. mix() allocates/frees nothing; finished leases release at collect_finished
// or on the control-side steal/destruction. No device or real-time deadline claim.
class Mixer {
  struct Voice {
    ClipLease clip;
    Play play;
    std::uint64_t generation{}, serial{}, position{}, stop_frame{UINT64_MAX};
    double left{1}, right{1};
    bool started{}, finished{};
  };
  std::array<Voice,max_voices> voices_{};
  std::array<double,max_buses> buses_{1,1,1,1};
  Limits limits_{};
  Listener listener_{};
  Vec3 origin_{};
  std::uint64_t owner_{}, frame_{}, serial_{};
  Statistics stats_{};
  Result<std::size_t> resolve(VoiceHandle) const noexcept;
public:
  Mixer() noexcept;
  Mixer(const Mixer&) = delete;
  Mixer& operator=(const Mixer&) = delete;
  Result<void> set_limits(Limits) noexcept; // only with no retained voices
  Result<VoiceHandle> play(ClipLease, Play = {}) noexcept;
  Result<void> stop(VoiceHandle, std::uint64_t exclusive_frame) noexcept;
  Result<void> set_bus_gain(std::uint8_t, double) noexcept;
  Result<void> set_listener(Listener) noexcept;
  Result<void> set_position(VoiceHandle, Vec3, std::uint64_t origin_revision) noexcept;
  Result<void> rebase(Vec3 new_absolute_origin, std::uint64_t new_revision) noexcept;
  Result<MixReport> mix(std::span<std::int16_t> interleaved_stereo) noexcept;
  std::size_t collect_finished() noexcept;
  std::uint64_t frame() const noexcept { return frame_; }
  Listener listener() const noexcept { return listener_; }
  Statistics statistics() const noexcept { return stats_; }
};
} // namespace race::audio
