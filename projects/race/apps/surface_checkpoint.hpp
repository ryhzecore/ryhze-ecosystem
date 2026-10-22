#pragma once
#include "content_project.hpp"

namespace race::surface_checkpoint {
inline constexpr std::size_t maximum_bytes=1024,maximum_product_bytes=8*1024*1024;
inline constexpr std::size_t maximum_buffer_bytes=maximum_bytes+2*maximum_product_bytes;
struct Definition {
  content_project::Kind kind{content_project::Kind::Texture};
  assets::AssetId id;
  assets::Digest index_revision{},recipe_revision{},source{},cooked_revision{};
  friend bool operator==(const Definition&,const Definition&)=default;
};
struct Published {Definition definition;assets::Digest revision;std::string leaf;};
class Loaded {
  Definition definition_{};
  assets::Digest revision_{};
  std::vector<std::byte> manifest_,product_,texture_;
  Loaded()=default;
  void swap(Loaded&) noexcept;
  friend Result<Loaded> load(platform::RootedFiles&,std::string_view,const content_project::Snapshot&,platform::RootedFiles&);
public:
  Loaded(const Loaded&)=delete;
  Loaded& operator=(const Loaded&)=delete;
  Loaded(Loaded&&) noexcept;
  Loaded& operator=(Loaded&&) noexcept;
  const Definition& definition() const noexcept{return definition_;}
  assets::Digest revision() const noexcept{return revision_;}
  std::span<const std::byte> manifest() const noexcept{return manifest_;}
  std::span<const std::byte> product() const noexcept{return product_;}
  std::span<const std::byte> texture() const noexcept{return texture_;}
  std::size_t retained_bytes() const noexcept{return sizeof(Loaded)+manifest_.capacity()+product_.capacity()+texture_.capacity();}
};
Result<std::string> encode(const Definition&);
Result<Definition> decode(std::span<const std::byte>);
// Caller retains unchanged cooked bytes through the call. Exact readback and
// the full pinned texture closure are admitted before immutable manifest write.
// A failed late readback may leave an immutable file, never a Scene mutation.
Result<Published> capture(const content_project::Snapshot&,std::span<const std::byte> cooked,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints);
// Retains exact admitted material/texture bytes. No source import, latest lookup,
// recanonicalized index, or render/Scene mutation. Each product capacity <=8 MiB;
// manifest <=1 KiB, aggregate buffer capacity <=16 MiB+1 KiB, plus Loaded itself.
Result<Loaded> load(platform::RootedFiles& checkpoints,std::string_view leaf,
    const content_project::Snapshot&,platform::RootedFiles& products);
} // namespace race::surface_checkpoint
