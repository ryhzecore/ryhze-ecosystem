#pragma once
#include "mesh_recipe.hpp"
#include "project_index.hpp"

namespace race::mesh_project {
using project_index::maximum_entries;using project_index::maximum_bytes;
using project_index::valid_index_leaf;
inline constexpr std::size_t maximum_retained_bytes=256*1024;
struct Reference {assets::AssetId id;std::string recipe;assets::Digest revision;};
struct Resolved {Reference reference;mesh_recipe::Snapshot recipe;};
struct Storage {std::size_t index_capacity{},entry_capacity{},retained_bytes{};};
// Immutable after admission. All references are resolved and checked before any
// lookup succeeds. Keep this owner alive through every consumer's quiescence.
class Snapshot {
  std::vector<std::byte> bytes_;
  std::vector<Resolved> entries_;
  assets::Digest revision_{};
  std::size_t retained_{};
  Snapshot() noexcept:retained_(sizeof(Snapshot)){}
  void swap(Snapshot&) noexcept;
public:
  Snapshot(const Snapshot&)=delete;
  Snapshot& operator=(const Snapshot&)=delete;
  Snapshot(Snapshot&&) noexcept;
  Snapshot& operator=(Snapshot&&) noexcept;
  static Result<Snapshot> open(std::span<const std::byte>,platform::RootedFiles&,std::size_t retained_limit=maximum_retained_bytes);
  Result<const Resolved*> find(assets::AssetId) const noexcept;
  std::span<const Resolved> entries() const noexcept{return entries_;}
  std::span<const std::byte> bytes() const noexcept{return bytes_;}
  assets::Digest revision() const noexcept{return revision_;}
  std::size_t retained_bytes() const noexcept{return retained_;}
  Storage storage() const noexcept{return {bytes_.capacity(),entries_.capacity(),retained_};}
};
// The same RACEPRJ1 binary wire as typed Content, restricted to mesh references.
Result<std::vector<std::byte>> encode(std::span<const Reference>);
Result<Snapshot> load(platform::RootedFiles&,std::string_view index_leaf,std::size_t retained_limit=maximum_retained_bytes);
} // namespace race::mesh_project
