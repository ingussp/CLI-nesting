#pragma once

#include "clinesting/model.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace clinesting {

// Store parsed options for the historical demonstration executable.
struct DemoCliOptions {
  int count;
  int threads;
  double bitmapResolutionMm;
  int bitmapSearchStepPx;
  bool debugPlacement;
  std::optional<std::filesystem::path> outputPath;
  bool showHelp{false};
};

// Validate and translate demonstration command-line options.
DemoCliOptions parseDemoCliOptions(const std::vector<std::string_view>& args);

}  // namespace clinesting
