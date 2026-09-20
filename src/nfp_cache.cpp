#include "clinesting/nfp_cache.hpp"

#include <cmath>
#include <functional>
#include <shared_mutex>

namespace clinesting {

namespace {

// Quantize a rotation for consistent no-fit cache keys.
inline long long rotKey(double v) {
  return static_cast<long long>(std::llround(v * 1000000.0));
}

}  // namespace

// Compare all fields that determine the cached no-fit geometry.
bool NfpKey::operator==(const NfpKey& other) const {
  return A == other.A && B == other.B && rotKey(Arotation) == rotKey(other.Arotation) &&
         rotKey(Brotation) == rotKey(other.Brotation) && inner == other.inner;
}

// Combine shape and orientation fields into a no-fit key hash.
std::size_t NfpKeyHash::operator()(const NfpKey& key) const {
  std::size_t h1 = std::hash<std::string>{}(key.A);
  std::size_t h2 = std::hash<std::string>{}(key.B);
  std::size_t h3 = std::hash<long long>{}(rotKey(key.Arotation));
  std::size_t h4 = std::hash<long long>{}(rotKey(key.Brotation));
  std::size_t h5 = std::hash<bool>{}(key.inner);
  return (((h1 * 1315423911u) ^ h2) * 2654435761u) ^ h3 ^ (h4 << 1) ^ (h5 << 2);
}

// Initialize an independent geometry cache.
NfpCache::NfpCache(const NfpCache& other) {
  std::shared_lock lock(other.mutex_);
  outer_ = other.outer_;
  inner_ = other.inner_;
  outerStoreCount_ = other.outerStoreCount_;
  innerStoreCount_ = other.innerStoreCount_;
}

// Define assignment behavior for this resource-owning object.
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

// Check whether the cache contains the requested geometry.
bool NfpCache::has(const NfpKey& key) const {
  std::shared_lock lock(mutex_);
  if (key.inner) {
    return inner_.find(key) != inner_.end();
  }
  return outer_.find(key) != outer_.end();
}

// Retrieve a cached outer no-fit polygon if available.
std::optional<Polygon> NfpCache::findOuter(const NfpKey& key) const {
  std::shared_lock lock(mutex_);
  auto it = outer_.find(key);
  if (it == outer_.end()) {
    return std::nullopt;
  }
  return it->second;
}

// Retrieve cached feasible inner-placement polygons if available.
std::optional<std::vector<Polygon>> NfpCache::findInner(const NfpKey& key) const {
  std::shared_lock lock(mutex_);
  auto it = inner_.find(key);
  if (it == inner_.end()) {
    return std::nullopt;
  }
  return it->second;
}

// Store an outer no-fit polygon under its geometry key.
void NfpCache::insertOuter(const NfpKey& key, const Polygon& nfp) {
  std::unique_lock lock(mutex_);
  const auto [_, inserted] = outer_.try_emplace(key, nfp);
  if (inserted) {
    ++outerStoreCount_;
  }
}

// Store feasible inner-placement polygons under their geometry key.
void NfpCache::insertInner(const NfpKey& key, const std::vector<Polygon>& nfp) {
  std::unique_lock lock(mutex_);
  const auto [_, inserted] = inner_.try_emplace(key, nfp);
  if (inserted) {
    ++innerStoreCount_;
  }
}

// Return the number of cached outer geometry entries.
size_t NfpCache::outerStoreCount() const {
  std::shared_lock lock(mutex_);
  return outerStoreCount_;
}

// Return the number of cached inner geometry entries.
size_t NfpCache::innerStoreCount() const {
  std::shared_lock lock(mutex_);
  return innerStoreCount_;
}

}  // namespace clinesting
