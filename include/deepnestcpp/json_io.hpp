#pragma once
#include "deepnestcpp/orchestrator.hpp"
#include <filesystem>
#include <string_view>

namespace deepnest {
BackgroundRequest parseNestingJson(std::string_view text);
BackgroundRequest readNestingJson(const std::filesystem::path& path);
void writeNestingJson(const std::filesystem::path& path, const BackgroundRequest& input,
                      const OrchestratorRunStats& result);
}
