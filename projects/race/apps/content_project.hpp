#pragma once
#include "mesh_recipe.hpp"
#include "project_index.hpp"
#include "surface_recipe.hpp"
#include <variant>

namespace race::content_project {
using project_index::maximum_entries;using project_index::maximum_bytes;
using project_index::binary_version;using project_index::binary_header_bytes;
using project_index::binary_entry_bytes;using project_index::binary_checksum_bytes;
using project_index::Kind;using project_index::valid_index_leaf;
inline constexpr std::size_t maximum_retained_bytes=256*1024;
enum class Format : std::uint8_t {Binary=2};
struct Reference {assets::AssetId id;Kind kind{Kind::Mesh};std::string recipe;assets::Digest revision;};
using Recipe=std::variant<mesh_recipe::Snapshot,surface_recipe::Snapshot>;
struct Resolved {Reference reference;Recipe recipe;};
struct Storage {std::size_t index_capacity{},entry_capacity{},retained_bytes{};};
// Owns exact raw index/recipe bytes. No lookup is exposed until the entire typed
// closure has passed identity, revision, kind/profile and retained-capacity checks.
// Binary wire version 1 represents logical typed schema 2. JSON project indexes
// are rejected; recipe bytes remain JSON and are never reserialized on admission.
class Snapshot {
  std::vector<std::byte> bytes_;
  std::vector<Resolved> entries_;
  assets::Digest revision_{};
  std::size_t retained_{};
  std::uint32_t schema_{};
  Snapshot() noexcept:retained_(sizeof(Snapshot)){}
  void swap(Snapshot&) noexcept;
public:
  Snapshot(const Snapshot&)=delete;
  Snapshot& operator=(const Snapshot&)=delete;
  Snapshot(Snapshot&&) noexcept;
  Snapshot& operator=(Snapshot&&) noexcept;
  static Result<Snapshot> open(std::span<const std::byte>,platform::RootedFiles&,
      std::size_t retained_limit=maximum_retained_bytes);
  Result<const Resolved*> find(assets::AssetId) const noexcept;
  std::span<const Resolved> entries() const noexcept{return entries_;}
  std::span<const std::byte> bytes() const noexcept{return bytes_;}
  assets::Digest revision() const noexcept{return revision_;}
  std::uint32_t schema() const noexcept{return schema_;}
  Format format() const noexcept{return Format::Binary;}
  std::size_t retained_bytes() const noexcept{return retained_;}
  Storage storage() const noexcept{return {bytes_.capacity(),entries_.capacity(),retained_};}
};
// Version 1: fixed little-endian header/records, canonical zero-padded leaves,
// sorted unique IDs and SHA-256 of the entire header/record payload.
Result<std::vector<std::byte>> encode_binary(std::span<const Reference>);
bool valid_new_index_leaf(std::string_view) noexcept;
// Decode requires binary magic and includes full dependency admission.
Result<Snapshot> decode(std::span<const std::byte>,platform::RootedFiles&,
    std::size_t retained_limit=maximum_retained_bytes);
Result<Snapshot> load(platform::RootedFiles&,std::string_view index_leaf,
    std::size_t retained_limit=maximum_retained_bytes);
} // namespace race::content_project
