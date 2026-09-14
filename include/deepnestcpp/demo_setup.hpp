#pragma once

#include "deepnestcpp/model.hpp"

#include <vector>

namespace deepnest {

inline constexpr int kDefaultDemoPartCount = 2000;
inline constexpr double kDemoSheetWidthMm = 1500.0;
inline constexpr double kDemoSheetHeightMm = 1500.0;

Polygon makeDemoStarPolygon();
std::vector<Polygon> makeDemoStarParts(int count);
Polygon makeDemoSheet();

}  // namespace deepnest
