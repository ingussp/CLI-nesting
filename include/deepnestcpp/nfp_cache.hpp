#pragma once

#include "deepnestcpp/model.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace deepnest {

struct NfpKey {
  std::string A;
  std::string B;
  double Arotation{0.0};
  double Brotation{0.0};
  bool inner{false};

  bool operator==(const NfpKey& other) const;
};

struct NfpKeyHash {
  std::size_t operator()(const NfpKey& key) const;
};

class NfpCache {
 public:
  NfpCache() = default;
  NfpCache(const NfpCache& other);
  NfpCache& operator=(const NfpCache& other);

  bool has(const NfpKey& key) const;
  std::optional<Polygon> findOuter(const NfpKey& key) const;
  std::optional<std::vector<Polygon>> findInner(const NfpKey& key) const;
  void insertOuter(const NfpKey& key, const Polygon& nfp);
  void insertInner(const NfpKey& key, const std::vector<Polygon>& nfp);
  size_t outerStoreCount() const;
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

}  // namespace deepnest
