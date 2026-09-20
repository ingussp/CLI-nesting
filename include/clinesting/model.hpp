#pragma once

#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <functional>
#include <cstdint>

namespace clinesting {

enum class NestingAlgorithm { Nfp, Bitmap };
enum class SearchMode { First, Timed, Continuous };

// Return the detected logical CPU count with a one-worker fallback.
inline int defaultWorkerCount() {
  const unsigned int detected = std::thread::hardware_concurrency();
  return detected == 0U ? 1 : static_cast<int>(detected);
}

// Clamp an internal worker request to at least one worker.
inline int normalizeWorkerCount(int requested) {
  return requested >= 1 ? requested : 1;
}

// Represent one 2D contour vertex and its exactness flag.
struct Point {
  double x{0.0};
  double y{0.0};
  bool exact{true};
};

// Represent a finite edge between two contour points.
struct Segment {
  Point a;
  Point b;
};

// Store a contour, holes, identity, metadata and orientation rules.
struct Polygon {
  std::vector<Point> points;
  std::vector<Polygon> children;

  std::optional<int> id;
  std::string source;
  std::string filename;
  std::string geometryKey;
  std::string metadataJson;
  double rotation{0.0};
  // Absolute permitted orientations; empty keeps the global grid plus rotation.
  std::vector<double> allowedAngles;
};

// Represent an axis-aligned rectangle in nesting units.
struct Bounds {
  double x{0.0};
  double y{0.0};
  double width{0.0};
  double height{0.0};
};

// Collect search, clearance, precision and GPU settings.
struct Config {
  static constexpr int maxRotations = 3600;
  // Equally spaced angles over one full turn: 3600 means a 0.1 degree step.
  int rotations{4};
  int threads{defaultWorkerCount()};
  NestingAlgorithm algorithm{NestingAlgorithm::Nfp};
  double bitmapResolutionMm{1.0};
  int bitmapSearchStepPx{1};
  bool bitmapPreferAvx2{true};
  bool bitmapValidateGeometry{true};
  bool debugPlacement{false};
  double spacing{0.0};
  double sheetSpacing{0.0};
  double holeSpacing{0.0};
  bool simplify{false};
  std::string placementType{"gravity"};
  bool mergeLines{false};
  double clipperScale{10000000.0};
  double scale{1.0};
  double curveTolerance{0.3};
  double timeRatio{1.0};
  // Independent deterministic bitmap strategies. One keeps the compact search;
  // two additionally tries repeated pair/row patterns, concurrently when possible.
  int bitmapTrials{2};
  bool bitmapCacheRejects{true};
  bool bitmapPatternTrial{false}; // internal strategy selector
  bool gpuEnabled{false};
  int gpuDevice{-1}; // -1 chooses a discrete GPU when available
  bool gpuFallbackToCpu{true};
  int gpuBatchSize{65536};
  double timeLimitSeconds{0.0}; // 0 = unlimited; shared by every bitmap strategy/sheet
  bool continuous{false};
  SearchMode mode{SearchMode::First};
  double continuousRoundSeconds{30.0};
  uint64_t searchIteration{0}; // Internal restart variation; zero keeps the original strategies.
  std::function<bool()> stopRequested; // Internal, thread-safe cooperative cancellation hook.
};

// Describe the translation and orientation of one accepted part copy.
struct Placement {
  double x{0.0};
  double y{0.0};
  std::optional<int> id;
  double rotation{0.0};
  std::string source;
  std::string filename;
  double mergedLength{0.0};
  std::vector<Segment> mergedSegments;
};

// Group accepted part placements on a single stock sheet.
struct SheetPlacement {
  std::string sheet;
  std::optional<int> sheetid;
  std::vector<Placement> sheetplacements;
};

// Store used sheets, unplaced copies and aggregate layout statistics.
struct PlacementResult {
  std::vector<SheetPlacement> placements;
  double fitness{0.0};
  double area{0.0};
  double totalarea{0.0};
  double mergedLength{0.0};
  double utilisation{0.0};
  std::vector<Polygon> unplaced;
};

// Store the total length and segments of coincident cutting edges.
struct MergedLengthResult {
  double totalLength{0.0};
  std::vector<Segment> segments;
};

}  // namespace clinesting
