#include "clinesting/demo_cli.hpp"
#include "clinesting/demo_setup.hpp"
#include "clinesting/dxf_export.hpp"
#include "clinesting/orchestrator.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace clinesting;

namespace {

// Return the printable name of the chosen nesting algorithm.
const char* algorithmName(NestingAlgorithm algorithm) {
  return algorithm == NestingAlgorithm::Bitmap ? "bitmap" : "nfp";
}

// Print phase timings for the completed run.
void printTimingLine(const char* label, double ms) {
  std::cout << std::fixed << std::setprecision(3) << label << ": " << ms << " ms (" << (ms / 1000.0) << " s)\n";
}

// Count physical part instances across all used sheets.
size_t countPlacedParts(const PlacementResult& result) {
  size_t placed = 0;
  for (const auto& sheet : result.placements) {
    placed += sheet.sheetplacements.size();
  }
  return placed;
}

// Print demo progress, placement details and diagnostics.
class StdoutSink : public EventSink {
 public:
  // Record the worker count for progress reporting.
  void setWorkerCount(int workerCount) { workerCount_ = workerCount; }

  // Receive the start-of-job notification and input counts.
  void onTestStart(const std::vector<Polygon>& sheets,
                  const std::vector<Polygon>& parts,
                  const Config&,
                  int index) override {
    std::cout << "start index=" << index << " sheets=" << sheets.size() << " parts=" << parts.size() << "\n";
    std::cout << "workers=" << workerCount_ << "\n";
  }

  // Receive the current nesting progress notification.
  void onProgress(int index, double progress) override {
    if (progress < 0.0) {
      if (lastPercent_ != 100) {
      std::cout << "progress index=" << index << " value=1\n";
      lastPercent_ = 100;
      }
      return;
    }

    const int percent = std::clamp(static_cast<int>(progress * 100.0), 0, 100);
    if (percent == lastPercent_) {
      return;
    }
    lastPercent_ = percent;
    std::cout << "progress index=" << index << " value=" << progress << "\n";
  }

  // Receive the completed placement result.
  void onResult(const PlacementResult& result) override {
    std::cout << "result fitness=" << result.fitness << " placements=" << result.placements.size()
              << " utilisation=" << result.utilisation << "%\n";
  }

  // Receive bitmap diagnostics after processing a part.
  void onBitmapPartProgress(const BitmapNestingStats::PartStats& partStats, size_t totalParts) override {
    if (!debugPlacementEnabled_) {
      return;
    }
    std::ostringstream line;
    line << std::fixed << std::setprecision(3)
         << "bitmap part=" << partStats.processedPart << "/" << totalParts
         << " placed=" << partStats.placedParts
         << " unplaced=" << partStats.unplacedParts
         << " candidates=" << partStats.candidatesExamined
         << " bitmap_collisions=" << partStats.bitmapCollisions
         << " boundary_rejects=" << partStats.boundaryRejects
         << " vector_rejects=" << partStats.vectorValidationRejects
         << " elapsed_ms=" << partStats.elapsedMs;
    std::cout << line.str() << "\n" << std::flush;
  }

  // Enable or disable detailed demo placement output.
  void setDebugPlacementEnabled(bool enabled) { debugPlacementEnabled_ = enabled; }

 private:
  int workerCount_{1};
  int lastPercent_{-1};
  bool debugPlacementEnabled_{false};
};

}  // namespace

// Parse command-line arguments and run the selected nesting workflow.
int main(int argc, char** argv) {
  const auto appStart = std::chrono::steady_clock::now();
  std::vector<std::string_view> args;
  args.reserve(static_cast<size_t>(std::max(argc - 1, 0)));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  DemoCliOptions options{
      kDefaultDemoPartCount, defaultWorkerCount(), NestingAlgorithm::Nfp, 1.0, 1, false, std::nullopt, false};
  try {
    options = parseDemoCliOptions(args);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return 1;
  }

  if (options.showHelp) {
    std::cout << "Usage: clinesting_demo [--count <N>] [--threads <N>] [--algorithm nfp|bitmap]\n"
                 "                        [--bitmap-resolution <mm-per-pixel>] [--bitmap-step <px>]\n"
                 "                        [--debug-placement] [--output <path>] [--help]\n"
              << "  --count <N>     Number of identical star parts to generate (default "
              << kDefaultDemoPartCount << ")\n"
              << "  --threads <N>   Worker count for independent NFP precompute (default "
              << defaultWorkerCount() << ")\n"
              << "  --algorithm     Nesting algorithm: nfp or bitmap (default nfp)\n"
              << "  --bitmap-resolution <mm-per-pixel> Raster resolution for bitmap nesting (default 1.0)\n"
              << "  --bitmap-step <px> Bitmap candidate search step in pixels (default 1)\n"
              << "  --debug-placement Print per-part bitmap debug/progress lines\n"
              << "  --output <path> Export placed parts and the 1500x1500 mm sheet to DXF\n";
    return 0;
  }

  BackgroundRequest req;
  req.index = 1;
  req.config.placementType = "box";
  req.config.rotations = 4;
  req.config.threads = options.threads;
  req.config.algorithm = options.algorithm;
  req.config.bitmapResolutionMm = options.bitmapResolutionMm;
  req.config.bitmapSearchStepPx = options.bitmapSearchStepPx;
  req.config.debugPlacement = options.debugPlacement;
  req.individual.placement = makeDemoStarParts(options.count);
  req.individual.rotation.assign(req.individual.placement.size(), 0.0);
  req.ids.reserve(req.individual.placement.size());
  req.sources.reserve(req.individual.placement.size());
  req.children.resize(req.individual.placement.size());
  req.filenames.reserve(req.individual.placement.size());
  for (const auto& part : req.individual.placement) {
    req.ids.push_back(part.id);
    req.sources.push_back(part.source);
    req.filenames.push_back(part.filename);
  }

  req.sheets = {makeDemoSheet()};
  req.sheetids = {req.sheets.front().id};
  req.sheetsources = {req.sheets.front().source};
  req.sheetchildren = {{}};

  auto sourceParts = req.individual.placement;
  for (size_t i = 0; i < sourceParts.size(); ++i) {
    if (i < req.individual.rotation.size()) {
      sourceParts[i].rotation = req.individual.rotation[i];
    }
    if (i < req.ids.size()) {
      sourceParts[i].id = req.ids[i];
    }
    if (i < req.sources.size()) {
      sourceParts[i].source = req.sources[i];
    }
    if (i < req.filenames.size()) {
      sourceParts[i].filename = req.filenames[i];
    }
    if (!req.config.simplify && i < req.children.size()) {
      sourceParts[i].children = req.children[i];
    }
  }

  auto sourceSheets = req.sheets;
  for (size_t i = 0; i < sourceSheets.size(); ++i) {
    if (i < req.sheetids.size()) {
      sourceSheets[i].id = req.sheetids[i];
    }
    if (i < req.sheetsources.size()) {
      sourceSheets[i].source = req.sheetsources[i];
    }
    if (i < req.sheetchildren.size()) {
      sourceSheets[i].children = req.sheetchildren[i];
    }
  }

  StdoutSink sink;
  sink.setWorkerCount(req.config.threads);
  sink.setDebugPlacementEnabled(req.config.debugPlacement);
  BackgroundOrchestrator orchestrator;
  OrchestratorRunStats runStats;
  try {
    runStats = orchestrator.runWithStats(req, sink);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << "\n";
    return 1;
  }
  auto& result = runStats.placement;

  if (options.outputPath.has_value()) {
    try {
      const auto dxfStart = std::chrono::steady_clock::now();
      exportPlacementResultToDxf(*options.outputPath, sourceSheets, sourceParts, result);
      const auto dxfEnd = std::chrono::steady_clock::now();
      runStats.timings.dxfExportMs =
          std::chrono::duration<double, std::milli>(dxfEnd - dxfStart).count();
      std::cout << "DXF exported to: " << std::filesystem::absolute(*options.outputPath).string() << "\n";
    } catch (const std::exception& ex) {
      std::cerr << "Failed to export DXF: " << ex.what() << "\n";
      return 1;
    }
  }

  const size_t placedCount = countPlacedParts(result);
  std::cout << "algorithm: " << algorithmName(options.algorithm) << "\n";
  std::cout << "sheet size: " << kDemoSheetWidthMm << "x" << kDemoSheetHeightMm << " mm\n";
  std::cout << "part count: " << options.count << "\n";
  std::cout << "workers: " << options.threads << "\n";
  std::cout << "bitmap resolution: " << options.bitmapResolutionMm << " mm/pixel\n";
  std::cout << "bitmap search step: " << options.bitmapSearchStepPx << " px\n";
  std::cout << "bitmap SIMD backend: " << runStats.simdBackend << "\n";
  std::cout << "placed parts: " << placedCount << "\n";
  std::cout << "unplaced parts: " << result.unplaced.size() << "\n";
  std::cout << "placed sheets: " << result.placements.size() << "\n";
  std::cout << "utilisation: " << result.utilisation << "%\n";
  if (options.algorithm == NestingAlgorithm::Bitmap) {
    std::cout << "bitmap summary: processed=" << runStats.bitmapStats.processedParts
              << " placed=" << runStats.bitmapStats.placedParts
              << " unplaced=" << runStats.bitmapStats.unplacedParts
              << " candidates=" << runStats.bitmapStats.candidatesExamined
              << " boundary_rejects=" << runStats.bitmapStats.boundaryRejects
              << " bitmap_collisions=" << runStats.bitmapStats.bitmapCollisions
              << " vector_rejects=" << runStats.bitmapStats.vectorValidationRejects
              << " accepted=" << runStats.bitmapStats.acceptedPlacements
              << " mask_cache=" << runStats.bitmapStats.cachedMaskCount
              << " neighbour_checks=" << runStats.bitmapStats.neighbourGeometryChecks
              << " window_expansions=" << runStats.bitmapStats.searchWindowExpansions
              << " fine_fallbacks=" << runStats.bitmapStats.fineFallbacks
              << " exhausted_skips=" << runStats.bitmapStats.exhaustedShapeSkips
              << " total_ms=" << runStats.bitmapStats.totalBitmapMs
              << " simd=" << runStats.bitmapStats.simdBackend << "\n";
  }
  printTimingLine("timing.setup", runStats.timings.setupMs);
  printTimingLine("timing.nfp_precompute", runStats.timings.nfpPrecomputeMs);
  printTimingLine("timing.placement", runStats.timings.placementMs);
  printTimingLine("timing.bitmap", runStats.timings.bitmapMs);
  printTimingLine("timing.dxf_export", runStats.timings.dxfExportMs);
  printTimingLine("timing.orchestrator_total", runStats.timings.totalMs);
  const double appTotalMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - appStart).count();
  printTimingLine("timing.total_with_output", appTotalMs);
  return result.placements.empty() ? 1 : 0;
}
