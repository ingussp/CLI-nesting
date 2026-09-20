#pragma once

#include "clinesting/model.hpp"

#include <vector>

namespace clinesting {

inline constexpr int kDefaultDemoPartCount = 2000;
inline constexpr double kDemoSheetWidthMm = 1500.0;
inline constexpr double kDemoSheetHeightMm = 1500.0;

// Construct the reusable star contour for demonstration jobs.
Polygon makeDemoStarPolygon();
// Generate uniquely identified copies of the demonstration star.
std::vector<Polygon> makeDemoStarParts(int count);
// Construct the stock polygon used by demonstration jobs.
Polygon makeDemoSheet();

}  // namespace clinesting
