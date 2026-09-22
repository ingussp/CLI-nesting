#pragma once

#include "clinesting/bitmap_nesting.hpp"
#include "clinesting/model.hpp"

#include <string>
#include <vector>

namespace clinesting {

// Store the ordered part instances and legacy rotation list.
struct IndividualInput {
  std::vector<Polygon> placement;
  std::vector<double> rotation;
};

// Combine stock, parts, output options and job metadata for a run.
struct BackgroundRequest {
  std::string jobId;
  std::string createdAt;
  std::string metadataJson;
  // Select output filenames, formats and preview opening.
  struct OutputOptions {
    std::string json{"result.json"};
    std::string dxf;
    std::string svg;
    bool openPreview{false};
  } output;
  int index{0};
  IndividualInput individual;

  std::vector<std::optional<int>> ids;
  std::vector<std::string> sources;
  std::vector<std::vector<Polygon>> children;
  std::vector<std::string> filenames;

  std::vector<Polygon> sheets;
  std::vector<std::optional<int>> sheetids;
  std::vector<std::string> sheetsources;
  std::vector<std::vector<Polygon>> sheetchildren;

  Config config;
};

// Expose progress and result notifications to nesting clients.
class EventSink {
 public:
  // Allow safe destruction through the event interface.
  virtual ~EventSink() = default;
  // Receive the start-of-job notification and input counts.
  virtual void onTestStart(const std::vector<Polygon>& sheets,
                           const std::vector<Polygon>& parts,
                           const Config& config,
                           int index) = 0;
  // Receive the current nesting progress notification.
  virtual void onProgress(int index, double progress) = 0;
  // Receive the completed placement result.
  virtual void onResult(const PlacementResult& result) = 0;
  // Receive bitmap diagnostics after processing a part.
  virtual void onBitmapPartProgress(const BitmapNestingStats::PartStats&, size_t) {}
};

// Store elapsed durations for orchestration and export phases.
struct NestingTimings {
  double setupMs{0.0};
  double placementMs{0.0};
  double bitmapMs{0.0};
  double dxfExportMs{0.0};
  double totalMs{0.0};
};

// Combine the winning layout with backend and timing diagnostics.
struct OrchestratorRunStats {
  uint64_t searchIteration{0};
  PlacementResult placement;
  NestingTimings timings;
  std::string simdBackend{"n/a"};
  BitmapNestingStats bitmapStats;
};

// Prepare input and report bitmap nesting results.
class BackgroundOrchestrator {
 public:
  BackgroundOrchestrator() = default;
  // Execute the requested nesting work and return its result.
  PlacementResult run(BackgroundRequest data, EventSink& sink);
  // Run nesting while retaining detailed timing and bitmap diagnostics.
  OrchestratorRunStats runWithStats(BackgroundRequest data, EventSink& sink,
                                  const BitmapLayoutCallback& onLayout = {});
};

}  // namespace clinesting
