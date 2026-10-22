#pragma once
#include "mesh_project.hpp"

namespace race::mesh_checkpoint {
namespace detail {struct Core;}
inline constexpr std::size_t maximum_bytes=1024,maximum_product_bytes=16*1024*1024;
struct Definition {
  assets::AssetId id;
  assets::Digest index_revision{},recipe_revision{},source{},cooked_revision{};
  friend bool operator==(const Definition&,const Definition&)=default;
};
struct Published {Definition definition;assets::Digest revision;std::string leaf;};
class Loaded {
  Definition definition_{};
  assets::Digest revision_{};
  std::vector<std::byte> manifest_,product_;
  Loaded()=default;
  void swap(Loaded&) noexcept;
  friend struct detail::Core;
public:
  Loaded(const Loaded&)=delete;
  Loaded& operator=(const Loaded&)=delete;
  Loaded(Loaded&&) noexcept;
  Loaded& operator=(Loaded&&) noexcept;
  const Definition& definition() const noexcept{return definition_;}
  assets::Digest revision() const noexcept{return revision_;}
  std::span<const std::byte> manifest() const noexcept{return manifest_;}
  std::span<const std::byte> product() const noexcept{return product_;}
  std::size_t retained_bytes() const noexcept{return sizeof(Loaded)+manifest_.capacity()+product_.capacity();}
};
Result<std::string> encode(const Definition&);
Result<Definition> decode(std::span<const std::byte>);
// Takes its own copy of a valid Store lease and retains it through all IO.
// Capture is independent of current-product admission; failed readback can leave
// a complete immutable file present, but never replaces an earlier checkpoint.
Result<Published> capture(const mesh_project::Snapshot&,assets::mesh::Lease,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints,const assets::mesh::Limits& = {});
// Explicit checksum-named checkpoint, exact project/recipe pins, validated cooked
// closure with the exact profile of the pinned project recipe. No source import
// or implicit latest-product/profile discovery occurs here.
Result<Loaded> load(platform::RootedFiles& checkpoints,std::string_view leaf,
    const mesh_project::Snapshot&,platform::RootedFiles& products,const assets::mesh::Limits& = {});
} // namespace race::mesh_checkpoint
