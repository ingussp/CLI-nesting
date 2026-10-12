#pragma once

#include "clinesting/model.hpp"
#include "clinesting/nfp_cache.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace clinesting {

// Build the enclosing frame used to derive inner no-fit polygons.
Polygon getFrame(const Polygon& A);

// Compute or retrieve forbidden relative positions for two polygons.
std::optional<Polygon> getOuterNfp(const Polygon& A,
                                   const Polygon& B,
                                   bool inside,
                                   const Config& config,
                                   NfpCache& cache);

// Compute or retrieve feasible positions inside a container polygon.
std::optional<std::vector<Polygon>> getInnerNfp(const Polygon& A,
                                                const Polygon& B,
                                                const Config& config,
                                                NfpCache& cache);

// Identify a shape/orientation pair requiring no-fit preprocessing.
struct NfpPair {
  Polygon A;
  Polygon B;
  double Arotation{0.0};
  double Brotation{0.0};
  std::string Asource;
  std::string Bsource;
};

// Populate cached no-fit geometry for missing polygon pairs.
std::vector<NfpPair> preprocessMissingPairs(const std::vector<Polygon>& parts, NfpCache& cache);

}  // namespace clinesting
