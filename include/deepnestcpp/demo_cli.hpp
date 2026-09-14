#pragma once

#include "deepnestcpp/model.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace deepnest {

struct DemoCliOptions {
  int count;
  int threads;
  NestingAlgorithm algorithm;
  double bitmapResolutionMm;
  int bitmapSearchStepPx;
  bool debugPlacement;
  std::optional<std::filesystem::path> outputPath;
  bool showHelp{false};
};

DemoCliOptions parseDemoCliOptions(const std::vector<std::string_view>& args);

}  // namespace deepnest
