#pragma once

#include "deepnestcpp/model.hpp"
#include "deepnestcpp/nfp_cache.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace deepnest {

Polygon getFrame(const Polygon& A);

std::optional<Polygon> getOuterNfp(const Polygon& A,
                                   const Polygon& B,
                                   bool inside,
                                   const Config& config,
                                   NfpCache& cache);

std::optional<std::vector<Polygon>> getInnerNfp(const Polygon& A,
                                                const Polygon& B,
                                                const Config& config,
                                                NfpCache& cache);

struct NfpPair {
  Polygon A;
  Polygon B;
  double Arotation{0.0};
  double Brotation{0.0};
  std::string Asource;
  std::string Bsource;
};

std::vector<NfpPair> preprocessMissingPairs(const std::vector<Polygon>& parts, NfpCache& cache);

}  // namespace deepnest
