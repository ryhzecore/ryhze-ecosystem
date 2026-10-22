#pragma once
// Qualification instrumentation only. This is not an admitted production SDK
// allocator: the source/lifetime/fragmentation proof remains a separate gate.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <span>

namespace qualification {
class Arena {
  static constexpr std::size_t none=std::numeric_limits<std::size_t>::max();
  static constexpr std::uint64_t marker=0x524143454152454eULL;
  struct alignas(16) Block {
    std::size_t bytes{},previous{none},next{none},payload{},requested{};
    std::uint64_t magic{marker};
    std::uint64_t used{},reserved{};
  };
  std::span<std::byte> memory_;
  Block& at(std::size_t offset) noexcept {return *std::launder(reinterpret_cast<Block*>(memory_.data()+offset));}
  void combine(std::size_t left) noexcept {
    auto& a=at(left);auto& b=at(a.next);
    a.bytes+=b.bytes;a.next=b.next;
    if(a.next!=none)at(a.next).previous=left;
  }
public:
  // ABI facts for the isolated source/lifetime ledger, not an admission claim.
  static constexpr std::size_t block_bytes=sizeof(Block),block_alignment=alignof(Block);
  struct Stats {
    std::size_t requests{},live_bytes{},peak_bytes{},live_blocks{},peak_blocks{},max_request{},max_alignment{};
  } stats;
  explicit Arena(std::span<std::byte> memory) noexcept : memory_(memory) {
    // The probe supplies page-aligned, nonempty Windows backing storage.
    auto* head=::new(memory_.data()) Block;
    head->bytes=memory_.size();
  }
  Arena(const Arena&)=delete;
  Arena& operator=(const Arena&)=delete;
  void* allocate(std::size_t count,std::size_t alignment) noexcept {
    count=std::max(count,std::size_t{1});alignment=std::max(alignment,alignof(Block));
    if((alignment&(alignment-1))||alignment>128||count>memory_.size())return nullptr;
    for(std::size_t offset=0;offset!=none;offset=at(offset).next) {
      auto& block=at(offset);if(block.used)continue;
      const auto payload=(offset+sizeof(Block)+sizeof(std::size_t)+alignment-1)&~(alignment-1);
      if(payload>memory_.size()||count>memory_.size()-payload)continue;
      const auto end=(payload+count+alignof(Block)-1)&~(alignof(Block)-1);
      if(end<offset||end-offset>block.bytes)continue;
      const auto needed=end-offset;
      if(block.bytes-needed>=sizeof(Block)+32) {
        const auto next=offset+needed;
        auto* tail=::new(memory_.data()+next) Block;
        tail->bytes=block.bytes-needed;tail->previous=offset;tail->next=block.next;
        if(tail->next!=none)at(tail->next).previous=next;
        block.next=next;block.bytes=needed;
      }
      block.used=true;block.payload=payload;block.requested=count;
      std::memcpy(memory_.data()+payload-sizeof(offset),&offset,sizeof(offset));
      ++stats.requests;++stats.live_blocks;stats.live_bytes+=block.bytes;
      stats.peak_blocks=std::max(stats.peak_blocks,stats.live_blocks);
      stats.peak_bytes=std::max(stats.peak_bytes,stats.live_bytes);
      stats.max_request=std::max(stats.max_request,count);stats.max_alignment=std::max(stats.max_alignment,alignment);
      return memory_.data()+payload;
    }
    return nullptr;
  }
  bool release(void* pointer) noexcept {
    if(!pointer)return true;
    const auto value=reinterpret_cast<std::uintptr_t>(pointer),base=reinterpret_cast<std::uintptr_t>(memory_.data());
    if(value<base+sizeof(Block)+sizeof(std::size_t)||value>=base+memory_.size())return false;
    const auto payload=static_cast<std::size_t>(value-base);std::size_t offset{};
    std::memcpy(&offset,memory_.data()+payload-sizeof(offset),sizeof(offset));
    if(offset>memory_.size()-sizeof(Block)||offset%alignof(Block))return false;
    auto& block=at(offset);
    if(block.magic!=marker||!block.used||block.payload!=payload)return false;
    stats.live_bytes-=block.bytes;--stats.live_blocks;block.used=false;
    if(block.next!=none&&!at(block.next).used)combine(offset);
    if(block.previous!=none&&!at(block.previous).used)combine(block.previous);
    return true;
  }
};
} // namespace qualification
