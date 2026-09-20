#pragma once

#include "clinesting/model.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace clinesting {

// Collect bitmap search counters, worker usage and timing diagnostics.
struct BitmapNestingStats {
  std::string simdBackend{"scalar"};
  std::string gpuDevice;
  std::string gpuFallbackReason;
  size_t gpuCandidates{0};
  size_t gpuBatches{0};
  size_t cachedMaskCount{0};
  size_t acceptedPlacements{0};
  size_t processedParts{0};
  size_t placedParts{0};
  size_t unplacedParts{0};
  size_t candidatesExamined{0};
  size_t boundaryRejects{0};
  size_t bitmapCollisions{0};
  size_t vectorValidationRejects{0};
  double totalBitmapMs{0.0};
  // Store elapsed time for each bitmap search phase.
  struct PhaseTimings {
    double preparationMs{0}, proposalsMs{0}, searchMs{0}, refinementMs{0}, gpuMs{0};
    // Accumulate phase timings from another search result.
    PhaseTimings& operator+=(const PhaseTimings& b) {
      preparationMs+=b.preparationMs; proposalsMs+=b.proposalsMs;
      searchMs+=b.searchMs; refinementMs+=b.refinementMs; gpuMs+=b.gpuMs;
      return *this;
    }
  } phases;
  // Report the outcome and search effort for one processed copy.
  struct PartStats {
    size_t processedPart{0};
    bool placed{false};
    size_t placedParts{0};
    size_t unplacedParts{0};
    double elapsedMs{0.0};
    size_t candidatesExamined{0};
    size_t boundaryRejects{0};
    size_t bitmapCollisions{0};
    size_t vectorValidationRejects{0};
  };
  std::vector<PartStats> perPart;
  size_t neighbourGeometryChecks{0};
  size_t searchWindowExpansions{0};
  size_t fineFallbacks{0};
  size_t exhaustedShapeSkips{0};
  size_t rejectedPositionSkips{0};
  size_t patternPlacements{0};
  size_t completedTrials{1};
  size_t workersUsed{1};
  size_t proposalWorkersPerTrial{1};
  double occupiedBoundsArea{0.0};
  // Report one strategy's placement count and phase timings.
  struct TrialStats {
    std::string strategy;
    size_t placed{0};
    size_t patternPlacements{0};
    double elapsedMs{0.0};
    PhaseTimings phases;
    bool completed{true};
  };
  std::vector<TrialStats> trials;
  size_t selectedTrial{0};
  bool timeLimitReached{false};
  bool cancelled{false};
  size_t startedTrials{0};
};

using BitmapPartProgressCallback = std::function<void(const BitmapNestingStats::PartStats&, size_t totalParts)>;
// Serialized callback from search workers, after each finished/interrupted strategy.
using BitmapLayoutCallback = std::function<void(const PlacementResult&, const BitmapNestingStats&)>;

// Run bitmap strategies and retain the best validated layout.
PlacementResult placePartsBitmap(const std::vector<Polygon>& sheets,
                                 const std::vector<Polygon>& parts,
                                 const Config& config,
                                 BitmapNestingStats* stats = nullptr,
                                 const BitmapPartProgressCallback& onPartProgress = {},
                                 const BitmapLayoutCallback& onLayout = {});

// Report whether this build and CPU can use AVX2 bitmap operations.
bool bitmapAvx2Supported();

}  // namespace clinesting
