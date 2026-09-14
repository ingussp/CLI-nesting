#pragma once

#include "deepnestcpp/model.hpp"
#include "deepnestcpp/nfp_cache.hpp"

#include <functional>
#include <vector>

namespace deepnest {

using ProgressCallback = std::function<void(double)>;

PlacementResult placeParts(std::vector<Polygon> sheets,
                           std::vector<Polygon> parts,
                           const Config& config,
                           NfpCache& cache,
                           ProgressCallback progress = {});

}  // namespace deepnest
