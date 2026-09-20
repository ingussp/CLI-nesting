#pragma once
#include "clinesting/orchestrator.hpp"
#include <filesystem>
#include <string_view>

namespace clinesting {
// Parse FreeCAD or CLI JSON and validate the complete job.
BackgroundRequest parseNestingJson(std::string_view text);
// Read a size-limited JSON input file and parse the job.
BackgroundRequest readNestingJson(const std::filesystem::path& path);
// Export transformed contours, metadata and search diagnostics as JSON.
void writeNestingJson(const std::filesystem::path& path, const BackgroundRequest& input,
                      const OrchestratorRunStats& result);
}
