#pragma once

#include "clinesting/model.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace clinesting {

// Identify cached geometry by shapes, orientations and inner/outer mode.
struct NfpKey {
  std::string A;
  std::string B;
  double Arotation{0.0};
  double Brotation{0.0};
  bool inner{false};

  // Compare all fields that determine the cached no-fit geometry.
  bool operator==(const NfpKey& other) const;
};

// Hash a no-fit cache key for unordered lookup.
struct NfpKeyHash {
  // Combine shape and orientation fields into a no-fit key hash.
  std::size_t operator()(const NfpKey& key) const;
};

// Store reusable inner and outer no-fit polygons with synchronized access.
class NfpCache {
 public:
  // Initialize an independent geometry cache.
  NfpCache() = default;
  // Store reusable inner and outer no-fit polygons with synchronized access.
  NfpCache(const NfpCache& other);
  // Define assignment behavior for this resource-owning object.
  NfpCache& operator=(const NfpCache& other);

  // Check whether the cache contains the requested geometry.
  bool has(const NfpKey& key) const;
  // Retrieve a cached outer no-fit polygon if available.
  std::optional<Polygon> findOuter(const NfpKey& key) const;
  // Retrieve cached feasible inner-placement polygons if available.
  std::optional<std::vector<Polygon>> findInner(const NfpKey& key) const;
  // Store an outer no-fit polygon under its geometry key.
  void insertOuter(const NfpKey& key, const Polygon& nfp);
  // Store feasible inner-placement polygons under their geometry key.
  void insertInner(const NfpKey& key, const std::vector<Polygon>& nfp);
  // Return the number of cached outer geometry entries.
  size_t outerStoreCount() const;
  // Return the number of cached inner geometry entries.
  size_t innerStoreCount() const;

 private:
  // Return values are copied out while the shared lock is held so callers never observe
  // references that would become invalid after the lock is released.
  mutable std::shared_mutex mutex_;
  std::unordered_map<NfpKey, Polygon, NfpKeyHash> outer_;
  std::unordered_map<NfpKey, std::vector<Polygon>, NfpKeyHash> inner_;
  size_t outerStoreCount_{0};
  size_t innerStoreCount_{0};
};

}  // namespace clinesting
