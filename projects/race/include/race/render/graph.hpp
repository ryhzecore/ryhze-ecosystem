#pragma once
#include "race/render/resource_contract.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace race::render::graph {
inline constexpr std::size_t max_resources=64,max_passes=8,max_uses=128,max_transitions=256;
using ResourceId=std::uint16_t;
using PassId=std::uint16_t;
using PassMask=std::uint16_t;
inline constexpr PassId no_pass=UINT16_MAX;
enum class Lifetime : std::uint8_t { Imported=1, Persistent=2, Transient=3 };
enum class Memory : std::uint8_t { Device=1, Upload=2, Readback=3 };
enum class State : std::uint8_t {
  Undefined=0, UploadRead=1, CopySource=2, CopyDestination=3, VertexRead=4,
  SampledRead=5, ColorAttachment=6, DepthAttachment=7, Present=8
};
enum class Access : std::uint8_t { Read=1, Write=2, ReadWrite=3 };
enum class Queue : std::uint8_t { Direct=1 };
enum class Operation : std::uint8_t {
  Upload=1, Opaque=2, GridSelection=3, Overlay=4, PresentCopy=5,
  TimingResolve=6, CaptureCopy=7,
  OutputTransform=8 // PhysicalDiffuseV1 only: reads the HDR target, writes the display image once.
};
struct Resource {
  ResourceDescription description{};
  Lifetime lifetime{Lifetime::Persistent};
  Memory memory{Memory::Device};
  State initial{State::CopyDestination},terminal{State::CopyDestination};
  // Imported validity certifies the logical bytes the admitted operations read.
  // The adapter proves active vertex bytes and copied mip rows; unused native
  // capacity and footprint padding are not initialized data or graph reads.
  bool content_valid{};
  bool exported{}; // Observed beyond this plan; never eligible for alias placement.
  // Trailing so existing positional initializers stay valid. General resources keep
  // the 64 MiB validate_resource ceiling; the HDR classes use validate_output_resource.
  OutputResourceClass output_class{OutputResourceClass::General};
};
struct Use {
  ResourceId resource{};
  Access access{Access::Read};
  State state{State::CopySource};
  bool initializes_all{}; // Access::Write only; complete logical write/clear.
};
struct Pass {
  Operation operation{Operation::Opaque};
  Queue queue{Queue::Direct};
  PassMask prerequisites{};
  std::uint16_t first_use{},use_count{};
};
struct Description {
  std::span<const Resource> resources;
  std::span<const Pass> passes;
  std::span<const Use> uses;
};
struct Transition {
  ResourceId resource{};
  State before{},after{};
  friend bool operator==(const Transition&,const Transition&)=default;
};
struct Step {
  PassId pass{};
  std::uint16_t before_first{},before_count{},after_first{},after_count{};
};
struct LiveRange {
  PassId first{no_pass},last{no_pass};
  std::uint8_t first_order{},last_order{};
  bool complete_first_write{},content_valid_after{};
};
struct Scratch {
  std::array<PassId,max_resources> writers{};
  std::array<PassMask,max_resources> readers{};
  std::array<std::uint8_t,max_uses> use_owners{};
  std::array<State,max_resources> states{};
  std::array<bool,max_resources> valid{};
};
class Plan {
  std::array<Resource,max_resources> resources_{};
  std::array<Pass,max_passes> passes_{};
  std::array<Use,max_uses> uses_{};
  std::array<Step,max_passes> steps_{};
  std::array<PassMask,max_passes> dependencies_{};
  std::array<LiveRange,max_resources> lives_{};
  std::array<Transition,max_transitions> transitions_{};
  std::uint16_t resource_count_{},pass_count_{},use_count_{},transition_count_{};
  Plan()=default;
  friend Result<Plan> compile(const Description&,Scratch&) noexcept;
public:
  std::span<const Resource> resources() const noexcept { return {resources_.data(),resource_count_}; }
  std::span<const Pass> passes() const noexcept { return {passes_.data(),pass_count_}; }
  std::span<const Use> uses() const noexcept { return {uses_.data(),use_count_}; }
  std::span<const Step> steps() const noexcept { return {steps_.data(),pass_count_}; }
  std::span<const PassMask> dependencies() const noexcept { return {dependencies_.data(),pass_count_}; }
  std::span<const LiveRange> lifetimes() const noexcept { return {lives_.data(),resource_count_}; }
  std::span<const Transition> transitions() const noexcept { return {transitions_.data(),transition_count_}; }
};
// Whole resources, one direct queue, stable declaration-order versions. The plan
// owns all values; input spans are borrowed only for this call. No allocation,
// native calls or mutation of the description/active plan/submission state.
// Undefined -> state is a transient allocation/discard marker, not a native API
// transition. The current committed viewport never supplies Undefined.
Result<Plan> compile(const Description&,Scratch&) noexcept;

// Separate future-placement validation. Sizes/alignments come from the native
// allocation query, never tightly packed payload estimates. Heap class is an
// opaque backend compatibility token covering all native heap restrictions.
struct NativeAllocation {
  ResourceId resource{};
  std::uint64_t bytes{},alignment{};
  std::uint32_t heap_class{};
};
struct AliasHeap { std::uint64_t bytes{};std::uint32_t heap_class{}; };
struct AliasPlacement { ResourceId resource{};std::uint16_t heap{};std::uint64_t offset{}; };
struct AliasBoundary { ResourceId resource{};PassId before_pass{}; };
struct AliasValidation {
  std::uint64_t heap_bytes{},allocation_bytes{};
  std::uint16_t overlap_pairs{},boundary_count{};
  std::array<AliasBoundary,max_resources> boundaries{};
};
// Every non-exported transient device resource needs one allocation/placement;
// other resources cannot participate. Overlapping memory requires strictly
// disjoint compiled live intervals and a complete first write. Each returned
// boundary requires a native alias barrier before that resource's first use.
// This does not create heaps/resources, record barriers or report realized savings.
Result<AliasValidation> validate_alias_plan(const Plan&,std::span<const NativeAllocation>,
    std::span<const AliasHeap>,std::span<const AliasPlacement>,std::uint64_t budget) noexcept;
} // namespace race::render::graph
