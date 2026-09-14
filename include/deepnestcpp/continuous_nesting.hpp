#pragma once
#include "deepnestcpp/orchestrator.hpp"
#include <functional>

namespace deepnest {
struct LayoutQuality {
  size_t unplaced{0};
  double usedSheetWasteArea{0};
  double compactWasteArea{0};
};
LayoutQuality layoutQuality(const std::vector<Polygon>& sheets,const PlacementResult& result,
                            const BitmapNestingStats& stats);
bool improvesLayout(const LayoutQuality& candidate,const LayoutQuality& incumbent);

using ImprovementCallback=std::function<void(const BackgroundRequest&,const OrchestratorRunStats&,size_t)>;
// Blocks until the thread-safe stop predicate is true. Serializes improvement callbacks.
// Each restart varies part order and angle priority without changing allowed orientations.
void runContinuousNesting(BackgroundRequest request,const std::function<bool()>& stop,
                          const ImprovementCallback& onImprovement);
OrchestratorRunStats runTimedNesting(BackgroundRequest request,const std::function<bool()>& stop);
}
