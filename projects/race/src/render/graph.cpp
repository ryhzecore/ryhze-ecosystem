#include "race/render/graph.hpp"
#include <algorithm>
#include <limits>

namespace race::render::graph {
// At most one transition per use, plus one terminal transition per resource.
// Capacity checks below are defensive; admission already bounds their total.
static_assert(max_transitions >= max_uses + max_resources);
namespace {
PassMask bit(std::size_t pass) noexcept { return static_cast<PassMask>(1u << pass); }
bool readable_terminal(State s) noexcept {
  return s == State::UploadRead || s == State::CopySource || s == State::VertexRead ||
         s == State::SampledRead || s == State::Present;
}
bool supports(const Resource &r, State state) noexcept {
  const auto &d = r.description;
  if (state == State::Undefined)
    return r.lifetime == Lifetime::Transient && r.memory == Memory::Device && !r.content_valid;
  if (r.memory == Memory::Upload)
    return state == State::UploadRead && d.kind == ResourceKind::Buffer &&
           (d.uses & (CopySource | VertexInput | ConstantInput)) != 0;
  if (r.memory == Memory::Readback)
    return state == State::CopyDestination && d.kind == ResourceKind::Buffer &&
           (d.uses & CopyDestination) != 0;
  if (r.memory != Memory::Device)
    return false;
  switch (state) {
  case State::CopySource:
    return (d.uses & CopySource) != 0;
  case State::CopyDestination:
    return (d.uses & CopyDestination) != 0;
  case State::VertexRead:
    return d.kind == ResourceKind::Buffer && (d.uses & VertexInput) != 0;
  case State::SampledRead:
    return d.kind == ResourceKind::Texture2D && (d.uses & ShaderSample) != 0;
  case State::ColorAttachment:
    return d.kind == ResourceKind::Texture2D && (d.uses & ColorTarget) != 0;
  case State::DepthAttachment:
    return d.kind == ResourceKind::Texture2D && (d.uses & DepthTarget) != 0;
  case State::Present:
    return r.lifetime == Lifetime::Imported && d.kind == ResourceKind::Texture2D &&
           d.format != ResourceFormat::Depth32Float && d.mip_levels == 1 && (d.uses & CopyDestination) != 0;
  default:
    return false;
  }
}
bool compatible(const Use &u) noexcept {
  switch (u.state) {
  case State::UploadRead:
  case State::CopySource:
  case State::VertexRead:
  case State::SampledRead:
    return u.access == Access::Read;
  case State::CopyDestination:
    return u.access == Access::Write;
  case State::ColorAttachment:
    return u.access == Access::Write || u.access == Access::ReadWrite;
  case State::DepthAttachment:
    return u.access == Access::Read || u.access == Access::Write || u.access == Access::ReadWrite;
  default:
    return false;
  }
}
bool eligible(const Resource &r) noexcept {
  return r.lifetime == Lifetime::Transient && r.memory == Memory::Device && !r.exported;
}
Result<void> admit_resources(const Description &d, Scratch &scratch) noexcept {
  for (std::size_t i = 0; i < d.resources.size(); ++i) {
    const auto &r = d.resources[i];
    auto checked = validate_output_resource(r.description, r.output_class);
    if (!checked)
      return checked.error();
    if (r.output_class != OutputResourceClass::General && r.lifetime == Lifetime::Transient)
      return fail(ErrorCode::Unsupported, "HDR output resources are retained, never transient");
    if ((r.lifetime != Lifetime::Imported && r.lifetime != Lifetime::Persistent &&
         r.lifetime != Lifetime::Transient) ||
        (r.memory != Memory::Device && r.memory != Memory::Upload && r.memory != Memory::Readback))
      return fail(ErrorCode::Unsupported, "unsupported graph resource lifetime or memory class");
    if (r.memory != Memory::Device && r.description.kind != ResourceKind::Buffer)
      return fail(ErrorCode::InvalidInput, "upload and readback resources require buffers");
    if (r.lifetime == Lifetime::Transient && r.content_valid)
      return fail(ErrorCode::InvalidInput, "transient contents cannot be imported as valid");
    if (!supports(r, r.initial) || r.terminal == State::Undefined || !supports(r, r.terminal))
      return fail(ErrorCode::InvalidInput, "graph resource initial or terminal state is incompatible");
    scratch.states[i] = r.initial;
    scratch.valid[i] = r.content_valid;
  }
  return {};
}

Result<void> admit_dependencies(const Description &d, Scratch &scratch,
                                std::array<PassMask, max_passes> &dependencies) noexcept {
  const auto admitted_mask = static_cast<PassMask>((1u << d.passes.size()) - 1u);
  for (std::size_t p = 0; p < d.passes.size(); ++p) {
    const auto &pass = d.passes[p];
    const auto operation = static_cast<unsigned>(pass.operation);
    if (pass.queue != Queue::Direct || operation < 1u || operation > 8u)
      return fail(ErrorCode::Unsupported, "unsupported render graph queue or operation");
    if ((pass.prerequisites & static_cast<PassMask>(~admitted_mask)) != 0 ||
        (pass.prerequisites & bit(p)) != 0)
      return fail(ErrorCode::InvalidDependency, "render graph prerequisite is unknown or self-referential");
    if (!pass.use_count || pass.first_use > d.uses.size() || pass.use_count > d.uses.size() - pass.first_use)
      return fail(ErrorCode::InvalidInput, "render graph pass use range is invalid");
    dependencies[p] = pass.prerequisites;
    std::uint64_t seen{};
    for (std::size_t n = pass.first_use; n < std::size_t(pass.first_use) + pass.use_count; ++n) {
      if (scratch.use_owners[n] != UINT8_MAX)
        return fail(ErrorCode::InvalidInput, "render graph use belongs to multiple passes");
      scratch.use_owners[n] = static_cast<std::uint8_t>(p);
      const auto &u = d.uses[n];
      if (u.resource >= d.resources.size())
        return fail(ErrorCode::InvalidInput, "render graph resource index is invalid");
      const auto resource_bit = std::uint64_t{1} << u.resource;
      if ((seen & resource_bit) != 0)
        return fail(ErrorCode::InvalidInput, "duplicate resource use within one graph pass");
      seen |= resource_bit;
      if (!supports(d.resources[u.resource], u.state) || !compatible(u) ||
          (u.initializes_all && u.access != Access::Write))
        return fail(ErrorCode::InvalidInput, "render graph use state or access is incompatible");
      const auto writer = scratch.writers[u.resource];
      if (writer != no_pass)
        dependencies[p] |= bit(writer);
      if (u.access == Access::Read)
        scratch.readers[u.resource] |= bit(p);
      else {
        dependencies[p] |= scratch.readers[u.resource];
        scratch.writers[u.resource] = static_cast<PassId>(p);
        scratch.readers[u.resource] = 0;
      }
    }
  }
  for (std::size_t n = 0; n < d.uses.size(); ++n)
    if (scratch.use_owners[n] == UINT8_MAX)
      return fail(ErrorCode::InvalidInput, "render graph contains an orphan use");
  return {};
}

Result<void> order_passes(const Description &d, const std::array<PassMask, max_passes> &dependencies,
                          std::array<Step, max_passes> &steps,
                          std::array<LiveRange, max_resources> &lives) noexcept {
  PassMask scheduled{};
  for (std::size_t order = 0; order < d.passes.size(); ++order) {
    PassId selected = no_pass;
    for (std::size_t p = 0; p < d.passes.size(); ++p)
      if ((scheduled & bit(p)) == 0 && (dependencies[p] & static_cast<PassMask>(~scheduled)) == 0) {
        selected = static_cast<PassId>(p);
        break;
      }
    if (selected == no_pass)
      return fail(ErrorCode::Cycle, "render graph dependency cycle");
    scheduled |= bit(selected);
    steps[order].pass = selected;
    const auto &pass = d.passes[selected];
    for (std::size_t n = pass.first_use; n < std::size_t(pass.first_use) + pass.use_count; ++n) {
      const auto &u = d.uses[n];
      auto &live = lives[u.resource];
      if (live.first == no_pass) {
        live.first = selected;
        live.first_order = static_cast<std::uint8_t>(order);
        live.complete_first_write = u.access == Access::Write && u.initializes_all;
      }
      live.last = selected;
      live.last_order = static_cast<std::uint8_t>(order);
    }
  }
  for (std::size_t r = 0; r < d.resources.size(); ++r)
    if (lives[r].first == no_pass)
      return fail(ErrorCode::InvalidInput, "render graph resource is never used");
  return {};
}

Result<void> build_transitions(const Description &d, Scratch &scratch, std::array<Step, max_passes> &steps,
                               std::array<LiveRange, max_resources> &lives,
                               std::array<Transition, max_transitions> &transitions,
                               std::uint16_t &transition_count) noexcept {
  auto transition = [&](ResourceId resource, State after) noexcept {
    auto &before = scratch.states[resource];
    if (before == after)
      return true;
    if (transition_count == max_transitions)
      return false;
    transitions[transition_count++] = {resource, before, after};
    before = after;
    return true;
  };
  for (std::size_t order = 0; order < d.passes.size(); ++order) {
    auto &step = steps[order];
    const auto &pass = d.passes[step.pass];
    step.before_first = transition_count;
    for (std::size_t n = pass.first_use; n < std::size_t(pass.first_use) + pass.use_count; ++n) {
      const auto &u = d.uses[n];
      auto &valid = scratch.valid[u.resource];
      if (u.access != Access::Write && !valid)
        return fail(ErrorCode::InvalidInput, "render graph read requires initialized logical contents");
      if (u.initializes_all)
        valid = true;
      if (!transition(u.resource, u.state))
        return fail(ErrorCode::Exhausted, "render graph transition capacity exceeded");
    }
    step.before_count = static_cast<std::uint16_t>(transition_count - step.before_first);
    step.after_first = transition_count;
    for (std::size_t r = 0; r < d.resources.size(); ++r)
      if (lives[r].last_order == order) {
        const auto &resource = d.resources[r];
        if ((resource.exported || readable_terminal(resource.terminal)) && !scratch.valid[r])
          return fail(ErrorCode::InvalidInput, "render graph external read requires initialized contents");
        if (!transition(static_cast<ResourceId>(r), resource.terminal))
          return fail(ErrorCode::Exhausted, "render graph terminal transition capacity exceeded");
        lives[r].content_valid_after = scratch.valid[r];
      }
    step.after_count = static_cast<std::uint16_t>(transition_count - step.after_first);
  }
  return {};
}

Result<void> admit_alias_heaps(std::span<const AliasHeap> heaps, std::uint64_t budget,
                               AliasValidation &result) noexcept {
  for (const auto &heap : heaps) {
    if (!heap.bytes || !heap.heap_class)
      return fail(ErrorCode::InvalidInput, "invalid alias heap description");
    if (heap.bytes > std::numeric_limits<std::uint64_t>::max() - result.heap_bytes)
      return fail(ErrorCode::Overflow, "alias heap byte sum overflow");
    result.heap_bytes += heap.bytes;
    if (result.heap_bytes > budget)
      return fail(ErrorCode::Exhausted, "alias heap budget exceeded");
  }
  return {};
}

Result<void> admit_alias_allocations(std::span<const Resource> resources, std::span<const LiveRange> lives,
                                     std::span<const NativeAllocation> allocations,
                                     std::array<std::size_t, max_resources> &allocation_index,
                                     AliasValidation &result) noexcept {
  for (std::size_t i = 0; i < allocations.size(); ++i) {
    const auto &a = allocations[i];
    if (a.resource >= resources.size() || !eligible(resources[a.resource]) ||
        allocation_index[a.resource] != max_resources)
      return fail(ErrorCode::InvalidInput, "invalid or duplicate alias allocation resource");
    if (!a.bytes || !a.alignment || (a.alignment & (a.alignment - 1)) != 0 || !a.heap_class)
      return fail(ErrorCode::InvalidInput, "invalid native alias size, alignment or heap class");
    const auto payload =
        validate_output_resource(resources[a.resource].description, resources[a.resource].output_class);
    if (!payload)
      return payload.error();
    if (a.bytes < payload.value() || !lives[a.resource].complete_first_write)
      return fail(ErrorCode::InvalidInput, "alias allocation is undersized or lacks a complete first write");
    if (a.bytes > std::numeric_limits<std::uint64_t>::max() - result.allocation_bytes)
      return fail(ErrorCode::Overflow, "native alias allocation byte sum overflow");
    result.allocation_bytes += a.bytes;
    allocation_index[a.resource] = i;
  }
  return {};
}

Result<void> admit_alias_placements(std::span<const Resource> resources,
                                    std::span<const NativeAllocation> allocations,
                                    std::span<const AliasHeap> heaps,
                                    std::span<const AliasPlacement> placements,
                                    const std::array<std::size_t, max_resources> &allocation_index,
                                    std::array<std::size_t, max_resources> &placement_index,
                                    std::array<bool, max_resources> &used_heaps,
                                    std::array<std::uint64_t, max_resources> &ends) noexcept {
  for (std::size_t i = 0; i < placements.size(); ++i) {
    const auto &p = placements[i];
    if (p.resource >= resources.size() || allocation_index[p.resource] == max_resources ||
        placement_index[p.resource] != max_resources || p.heap >= heaps.size())
      return fail(ErrorCode::InvalidInput, "invalid or duplicate alias placement");
    const auto &a = allocations[allocation_index[p.resource]];
    const auto &heap = heaps[p.heap];
    if (a.heap_class != heap.heap_class || p.offset % a.alignment)
      return fail(ErrorCode::InvalidInput, "alias heap compatibility or placement alignment differs");
    if (a.bytes > std::numeric_limits<std::uint64_t>::max() - p.offset)
      return fail(ErrorCode::Overflow, "alias placement end overflow");
    ends[i] = p.offset + a.bytes;
    if (ends[i] > heap.bytes)
      return fail(ErrorCode::Exhausted, "alias placement exceeds heap bounds");
    placement_index[p.resource] = i;
    used_heaps[p.heap] = true;
  }
  for (std::size_t i = 0; i < heaps.size(); ++i)
    if (!used_heaps[i])
      return fail(ErrorCode::InvalidInput, "alias plan contains an unused heap");
  return {};
}

Result<void> admit_alias_overlaps(std::span<const LiveRange> lives,
                                  std::span<const AliasPlacement> placements,
                                  const std::array<std::uint64_t, max_resources> &ends,
                                  std::array<bool, max_resources> &barrier_required,
                                  AliasValidation &result) noexcept {
  for (std::size_t i = 0; i < placements.size(); ++i)
    for (std::size_t j = i + 1; j < placements.size(); ++j) {
      const auto &a = placements[i];
      const auto &b = placements[j];
      if (a.heap != b.heap || a.offset >= ends[j] || b.offset >= ends[i])
        continue;
      const auto &first = lives[a.resource];
      const auto &second = lives[b.resource];
      if (!(first.last_order < second.first_order || second.last_order < first.first_order))
        return fail(ErrorCode::InvalidInput, "alias memory overlaps live resource intervals");
      ++result.overlap_pairs;
      barrier_required[first.last_order < second.first_order ? b.resource : a.resource] = true;
    }
  return {};
}
} // namespace

Result<Plan> compile(const Description &d, Scratch &scratch) noexcept {
  if (d.resources.size() > max_resources || d.passes.size() > max_passes || d.uses.size() > max_uses)
    return fail(ErrorCode::Exhausted, "render graph description exceeds fixed capacity");
  if (d.resources.empty() || d.passes.empty() || d.uses.empty())
    return fail(ErrorCode::InvalidInput, "render graph requires resources and nonempty passes");
  Plan plan;
  plan.resource_count_ = static_cast<std::uint16_t>(d.resources.size());
  plan.pass_count_ = static_cast<std::uint16_t>(d.passes.size());
  plan.use_count_ = static_cast<std::uint16_t>(d.uses.size());
  std::copy(d.resources.begin(), d.resources.end(), plan.resources_.begin());
  std::copy(d.passes.begin(), d.passes.end(), plan.passes_.begin());
  std::copy(d.uses.begin(), d.uses.end(), plan.uses_.begin());
  scratch.writers.fill(no_pass);
  scratch.readers.fill(0);
  scratch.use_owners.fill(UINT8_MAX);
  auto admit_resources_result = admit_resources(d, scratch);
  if (!admit_resources_result)
    return admit_resources_result.error();
  auto admit_dependencies_result = admit_dependencies(d, scratch, plan.dependencies_);
  if (!admit_dependencies_result)
    return admit_dependencies_result.error();
  auto order_passes_result = order_passes(d, plan.dependencies_, plan.steps_, plan.lives_);
  if (!order_passes_result)
    return order_passes_result.error();
  auto build_transitions_result =
      build_transitions(d, scratch, plan.steps_, plan.lives_, plan.transitions_, plan.transition_count_);
  if (!build_transitions_result)
    return build_transitions_result.error();
  return plan;
}

Result<AliasValidation> validate_alias_plan(const Plan &plan, std::span<const NativeAllocation> allocations,
                                            std::span<const AliasHeap> heaps,
                                            std::span<const AliasPlacement> placements,
                                            std::uint64_t budget) noexcept {
  if (allocations.size() > max_resources || heaps.size() > max_resources || placements.size() > max_resources)
    return fail(ErrorCode::Exhausted, "render graph alias description exceeds fixed capacity");
  const auto resources = plan.resources();
  const auto lives = plan.lifetimes();
  std::size_t expected{};
  for (const auto &r : resources)
    if (eligible(r))
      ++expected;
  if (allocations.size() != expected || placements.size() != expected)
    return fail(ErrorCode::InvalidInput,
                "alias plan must cover every eligible transient resource exactly once");
  std::array<std::size_t, max_resources> allocation_index{}, placement_index{};
  allocation_index.fill(max_resources);
  placement_index.fill(max_resources);
  std::array<bool, max_resources> used_heaps{}, barrier_required{};
  std::array<std::uint64_t, max_resources> ends{};
  AliasValidation result;
  auto admit_alias_heaps_result = admit_alias_heaps(heaps, budget, result);
  if (!admit_alias_heaps_result)
    return admit_alias_heaps_result.error();
  auto admit_alias_allocations_result =
      admit_alias_allocations(resources, lives, allocations, allocation_index, result);
  if (!admit_alias_allocations_result)
    return admit_alias_allocations_result.error();
  auto admit_alias_placements_result = admit_alias_placements(
      resources, allocations, heaps, placements, allocation_index, placement_index, used_heaps, ends);
  if (!admit_alias_placements_result)
    return admit_alias_placements_result.error();
  auto admit_alias_overlaps_result = admit_alias_overlaps(lives, placements, ends, barrier_required, result);
  if (!admit_alias_overlaps_result)
    return admit_alias_overlaps_result.error();
  // Execution order, then resource ID, makes validation output deterministic.
  for (std::size_t order = 0; order < plan.steps().size(); ++order)
    for (std::size_t r = 0; r < resources.size(); ++r)
      if (barrier_required[r] && lives[r].first_order == order)
        result.boundaries[result.boundary_count++] = {static_cast<ResourceId>(r), lives[r].first};
  return result;
}
} // namespace race::render::graph
