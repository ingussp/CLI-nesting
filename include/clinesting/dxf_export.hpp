#pragma once

#include "clinesting/model.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace clinesting {

// Configure sheet separation, layers and labels in DXF output.
struct DxfExportOptions {
  bool includeLabels{true};
  double labelHeight{1.0};
};

// Resolves placements against source polygons by preferring (source + id), then id-only, then source-only
// in original input order. This avoids ambiguity when source strings are duplicated.
void exportPlacementResultToDxf(const std::filesystem::path& outputPath,
                                const std::vector<Polygon>& sourceSheets,
                                const std::vector<Polygon>& sourceParts,
                                const PlacementResult& result,
                                const DxfExportOptions& options = {});

}  // namespace clinesting
