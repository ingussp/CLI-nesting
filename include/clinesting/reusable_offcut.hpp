#pragma once
#include "clinesting/model.hpp"
namespace clinesting {
struct ReusableOffcut {
  bool evaluated{false};
  double area{0},freeArea{0},coreArea{0},minWidthMm{5},elapsedMs{0};
  double arcToleranceMm{0.001};
  double availableArea{0},clearanceExcludedArea{0};
  double spacingMm{0},sheetSpacingMm{0},holeSpacingMm{0};
  size_t components{0};
  std::optional<int> sheetId;
  Polygon contour; // One connected piece, with actual material holes retained.
};
// Split narrow connections by inward offset, then recover each component within
// the actual free material. Scores only used sheets; never counts untouched stock.
ReusableOffcut measureReusableOffcut(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const PlacementResult& layout,
    double minWidthMm=5,const std::function<bool()>& stop={});
// Reserve configured part, sheet-edge and hole clearances before measuring.
ReusableOffcut measureReusableOffcut(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const PlacementResult& layout,
    const Config& config,const std::function<bool()>& stop={});
}
