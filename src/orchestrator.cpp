#include "clinesting/bitmap_nesting.hpp"
#include "clinesting/orchestrator.hpp"

#include <chrono>
#include <vector>

namespace clinesting {

// Execute the requested nesting work and return its result.
PlacementResult BackgroundOrchestrator::run(BackgroundRequest data, EventSink& sink) {
  return runWithStats(std::move(data), sink).placement;
}

// Run nesting while retaining detailed timing and bitmap diagnostics.
OrchestratorRunStats BackgroundOrchestrator::runWithStats(BackgroundRequest data, EventSink& sink,
                                                        const BitmapLayoutCallback& onLayout) {
  const auto t0 = std::chrono::steady_clock::now();
  OrchestratorRunStats runStats;
  auto parts = data.individual.placement;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i < data.individual.rotation.size()) {
      parts[i].rotation = data.individual.rotation[i];
    }
    if (i < data.ids.size()) {
      parts[i].id = data.ids[i];
    }
    if (i < data.sources.size()) {
      parts[i].source = data.sources[i];
    }
    if (i < data.filenames.size()) {
      parts[i].filename = data.filenames[i];
    }
    if (!data.config.simplify && i < data.children.size()) {
      parts[i].children = data.children[i];
    }
  }

  auto sheets = data.sheets;
  for (size_t i = 0; i < sheets.size(); ++i) {
    if (i < data.sheetids.size()) {
      sheets[i].id = data.sheetids[i];
    }
    if (i < data.sheetsources.size()) {
      sheets[i].source = data.sheetsources[i];
    }
    if (i < data.sheetchildren.size()) {
      sheets[i].children = data.sheetchildren[i];
    }
  }
  const auto tSetupEnd = std::chrono::steady_clock::now();
  runStats.timings.setupMs =
      std::chrono::duration<double, std::milli>(tSetupEnd - t0).count();

  sink.onTestStart(sheets, parts, data.config, data.index);
  sink.onProgress(data.index, 0.0);

  const auto tBitmapStart = std::chrono::steady_clock::now();
  BitmapNestingStats bitmapStats;
  runStats.placement =
      placePartsBitmap(sheets, parts, data.config, &bitmapStats, [&](const BitmapNestingStats::PartStats& partStats,
                                                                     size_t totalParts) {
        sink.onBitmapPartProgress(partStats, totalParts);
      }, onLayout);
  const auto tBitmapEnd = std::chrono::steady_clock::now();
  runStats.timings.bitmapMs =
      std::chrono::duration<double, std::milli>(tBitmapEnd - tBitmapStart).count();
  runStats.timings.placementMs = runStats.timings.bitmapMs;
  runStats.simdBackend = bitmapStats.simdBackend;
  runStats.bitmapStats = bitmapStats;
  sink.onProgress(data.index, -1.0);

  sink.onResult(runStats.placement);
  runStats.timings.totalMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return runStats;
}

}  // namespace clinesting
