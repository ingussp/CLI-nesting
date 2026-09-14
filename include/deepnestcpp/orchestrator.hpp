#pragma once

#include "deepnestcpp/bitmap_nesting.hpp"
#include "deepnestcpp/model.hpp"
#include "deepnestcpp/nfp_cache.hpp"

#include <string>
#include <vector>

namespace deepnest {

struct IndividualInput {
  std::vector<Polygon> placement;
  std::vector<double> rotation;
};

struct BackgroundRequest {
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

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void onTestStart(const std::vector<Polygon>& sheets,
                           const std::vector<Polygon>& parts,
                           const Config& config,
                           int index) = 0;
  virtual void onProgress(int index, double progress) = 0;
  virtual void onResult(const PlacementResult& result) = 0;
  virtual void onBitmapPartProgress(const BitmapNestingStats::PartStats&, size_t) {}
};

struct NestingTimings {
  double setupMs{0.0};
  double nfpPrecomputeMs{0.0};
  double placementMs{0.0};
  double bitmapMs{0.0};
  double dxfExportMs{0.0};
  double totalMs{0.0};
};

struct OrchestratorRunStats {
  uint64_t searchIteration{0};
  PlacementResult placement;
  NestingTimings timings;
  std::string simdBackend{"n/a"};
  BitmapNestingStats bitmapStats;
};

class BackgroundOrchestrator {
 public:
  explicit BackgroundOrchestrator(NfpCache cache = {});
  PlacementResult run(BackgroundRequest data, EventSink& sink);
  OrchestratorRunStats runWithStats(BackgroundRequest data, EventSink& sink,
                                  const BitmapLayoutCallback& onLayout = {});

 private:
  NfpCache cache_;
};

}  // namespace deepnest
