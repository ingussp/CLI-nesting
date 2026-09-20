#pragma once

#include "clinesting/model.hpp"
#include "clinesting/nfp_cache.hpp"

#include <functional>
#include <vector>

namespace clinesting {

using ProgressCallback = std::function<void(double)>;

// Place parts with the reference no-fit-polygon algorithm.
PlacementResult placeParts(std::vector<Polygon> sheets,
                           std::vector<Polygon> parts,
                           const Config& config,
                           NfpCache& cache,
                           ProgressCallback progress = {});

}  // namespace clinesting
