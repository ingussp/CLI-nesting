#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace clinesting::detail {
// One budget for all concurrently running strategies in a nesting invocation.
// Accounting includes page payload plus a conservative per-map-entry allowance.
class RejectionCacheBudget {
  size_t limit_;
  std::atomic<size_t> used_{0},peak_{0};
 public:
  explicit RejectionCacheBudget(size_t bytes):limit_(bytes) {}
  bool reserve(size_t bytes) {
    auto used=used_.load();
    while(used<=limit_ && bytes<=limit_-used) {
      if(used_.compare_exchange_weak(used,used+bytes)) {
        auto peak=peak_.load();
        while(peak<used+bytes&&!peak_.compare_exchange_weak(peak,used+bytes)) {}
        return true;
      }
    }
    return false;
  }
  void release(size_t bytes) {used_.fetch_sub(bytes);}
  size_t used() const {return used_.load();}
  size_t peak() const {return peak_.load();}
};

// Allocate only touched 4096-position tiles. Missing/full-budget tiles mean
// "unknown", never rejection. Reads may run concurrently; insertions occur
// only on the strategy owner after its parallel readers finish.
class RejectedOrigins {
  using Page=std::array<uint64_t,64>;
  static constexpr uint64_t pageBits=64*64;
  static constexpr size_t pageCost=sizeof(Page)+128;
  std::unordered_map<uint64_t,std::unique_ptr<Page>> pages_;
  RejectionCacheBudget* budget_{nullptr};
  size_t allocated_{0};
 public:
  uint64_t rows{0},rotations{0},size{0};
  RejectedOrigins()=default;
  RejectedOrigins(const RejectedOrigins&)=delete;
  RejectedOrigins& operator=(const RejectedOrigins&)=delete;
  ~RejectedOrigins() {if(budget_) budget_->release(allocated_);}
  void configure(RejectionCacheBudget* budget) {budget_=budget;}
  uint64_t index(int x,int y,size_t r) const {return (uint64_t(x)*rows+uint64_t(y))*rotations+r;}
  bool contains(uint64_t i) const {
    if(i>=size) return false;
    const auto it=pages_.find(i/pageBits);
    return it!=pages_.end() && ((*it->second)[(i%pageBits)/64]&(uint64_t(1)<<(i%64)));
  }
  void insert(uint64_t i) {
    if(!budget_||i>=size) return;
    auto it=pages_.find(i/pageBits);
    if(it==pages_.end()) {
      if(!budget_->reserve(pageCost)) return;
      try {it=pages_.emplace(i/pageBits,std::make_unique<Page>(Page{})).first;}
      catch(...) {budget_->release(pageCost);throw;}
      allocated_+=pageCost;
    }
    (*it->second)[(i%pageBits)/64]|=uint64_t(1)<<(i%64);
  }
  uint64_t next(uint64_t i) const {
    while(i<size) {
      const auto it=pages_.find(i/pageBits);
      if(it==pages_.end()) return i;
      const uint64_t available=~(*it->second)[(i%pageBits)/64]&(~uint64_t(0)<<(i%64));
      if(available) return std::min(size,(i/64)*64+std::countr_zero(available));
      i=(i/64+1)*64;
    }
    return size;
  }
};
} // namespace clinesting::detail
