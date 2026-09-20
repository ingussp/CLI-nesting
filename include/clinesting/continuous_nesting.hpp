#pragma once
#include "clinesting/orchestrator.hpp"
#include <functional>

namespace clinesting {
// Store the ordered objectives used to compare candidate layouts.
struct LayoutQuality {
  size_t unplaced{0};
  double usedSheetWasteArea{0};
  double compactWasteArea{0};
};
// Compute unplaced count and unused stock/bounding areas.
LayoutQuality layoutQuality(const std::vector<Polygon>& sheets,const PlacementResult& result,
                            const BitmapNestingStats& stats);
// Compare layouts by completeness, stock waste and compactness.
bool improvesLayout(const LayoutQuality& candidate,const LayoutQuality& incumbent);

using ImprovementCallback=std::function<void(const BackgroundRequest&,const OrchestratorRunStats&,size_t)>;
// Blocks until the thread-safe stop predicate is true. Serializes improvement callbacks.
// Each restart varies part order and angle priority without changing allowed orientations.
void runContinuousNesting(BackgroundRequest request,const std::function<bool()>& stop,
                          const ImprovementCallback& onImprovement);
// Optimize until the shared deadline and return the best candidate.
OrchestratorRunStats runTimedNesting(BackgroundRequest request,const std::function<bool()>& stop);
}
