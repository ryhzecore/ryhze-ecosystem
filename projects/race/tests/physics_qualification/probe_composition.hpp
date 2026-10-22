#pragma once
#include "owned_records.hpp"

// Private bridge between the exception-contained output owner and SDK probe.
// No native type or backing pointer crosses a public RACE service boundary.
namespace qualification::composition {
using namespace owned;
struct Control {
  std::array<BodyRecord,256> bodies{};
  std::array<PendingCommand,256> pending{};
  RemovalIdentities identities{};
  std::array<QuerySnapshotValue,256> previous{},candidate{};
  std::array<bool,32640> previous_pairs{},candidate_pairs{};
  alignas(16) std::array<std::byte,512> operand{};
};
struct Memory {
  std::byte* sdk{};
  std::byte* control{};
  PhaseValues* phase{};
  std::array<QueryValues*,64> queries{};
  std::size_t control_used{},sdk_peak{},scratch_peak{},events{},pair_tests{};
};
bool prepare_products(Control&) noexcept;
void execute_sdk(Memory&) noexcept;
} // namespace qualification::composition
void run_composed_ownership_probe();
