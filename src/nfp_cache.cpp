#include "deepnestcpp/nfp_cache.hpp"

#include <cmath>
#include <functional>
#include <shared_mutex>

namespace deepnest {

namespace {

inline long long rotKey(double v) {
  return static_cast<long long>(std::llround(v * 1000000.0));
}

}  // namespace

bool NfpKey::operator==(const NfpKey& other) const {
  return A == other.A && B == other.B && rotKey(Arotation) == rotKey(other.Arotation) &&
         rotKey(Brotation) == rotKey(other.Brotation) && inner == other.inner;
}

std::size_t NfpKeyHash::operator()(const NfpKey& key) const {
  std::size_t h1 = std::hash<std::string>{}(key.A);
  std::size_t h2 = std::hash<std::string>{}(key.B);
  std::size_t h3 = std::hash<long long>{}(rotKey(key.Arotation));
  std::size_t h4 = std::hash<long long>{}(rotKey(key.Brotation));
  std::size_t h5 = std::hash<bool>{}(key.inner);
  return (((h1 * 1315423911u) ^ h2) * 2654435761u) ^ h3 ^ (h4 << 1) ^ (h5 << 2);
}

NfpCache::NfpCache(const NfpCache& other) {
  std::shared_lock lock(other.mutex_);
  outer_ = other.outer_;
  inner_ = other.inner_;
  outerStoreCount_ = other.outerStoreCount_;
  innerStoreCount_ = other.innerStoreCount_;
}

NfpCache& NfpCache::operator=(const NfpCache& other) {
  if (this == &other) {
    return *this;
  }

  std::shared_lock otherLock(other.mutex_);
  std::unique_lock thisLock(mutex_);
  outer_ = other.outer_;
  inner_ = other.inner_;
  outerStoreCount_ = other.outerStoreCount_;
  innerStoreCount_ = other.innerStoreCount_;
  return *this;
}

bool NfpCache::has(const NfpKey& key) const {
  std::shared_lock lock(mutex_);
  if (key.inner) {
    return inner_.find(key) != inner_.end();
  }
  return outer_.find(key) != outer_.end();
}

std::optional<Polygon> NfpCache::findOuter(const NfpKey& key) const {
  std::shared_lock lock(mutex_);
  auto it = outer_.find(key);
  if (it == outer_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::vector<Polygon>> NfpCache::findInner(const NfpKey& key) const {
  std::shared_lock lock(mutex_);
  auto it = inner_.find(key);
  if (it == inner_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void NfpCache::insertOuter(const NfpKey& key, const Polygon& nfp) {
  std::unique_lock lock(mutex_);
  const auto [_, inserted] = outer_.try_emplace(key, nfp);
  if (inserted) {
    ++outerStoreCount_;
  }
}

void NfpCache::insertInner(const NfpKey& key, const std::vector<Polygon>& nfp) {
  std::unique_lock lock(mutex_);
  const auto [_, inserted] = inner_.try_emplace(key, nfp);
  if (inserted) {
    ++innerStoreCount_;
  }
}

size_t NfpCache::outerStoreCount() const {
  std::shared_lock lock(mutex_);
  return outerStoreCount_;
}

size_t NfpCache::innerStoreCount() const {
  std::shared_lock lock(mutex_);
  return innerStoreCount_;
}

}  // namespace deepnest
