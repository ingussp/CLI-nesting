#include "clinesting/bitmap_nesting.hpp"
#include "clinesting/orchestrator.hpp"

#include "clinesting/nfp.hpp"
#include "clinesting/placement.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace clinesting {

namespace {

// Schedule only geometry pairs that are absent from the cache.
void precomputeMissingPairs(const std::vector<NfpPair>& pairs, const Config& config, NfpCache& cache, int requestedThreads) {
  if (pairs.empty()) {
    return;
  }

  const size_t workerCount =
      std::min(pairs.size(), static_cast<size_t>(normalizeWorkerCount(requestedThreads)));
  if (workerCount <= 1) {
    for (const auto& pair : pairs) {
      static_cast<void>(getOuterNfp(pair.A, pair.B, false, config, cache));
    }
    return;
  }

  std::atomic<size_t> nextIndex{0};
  std::exception_ptr firstError;
  std::mutex errorMutex;
  std::vector<std::thread> workers;
  workers.reserve(workerCount);

  for (size_t worker = 0; worker < workerCount; ++worker) {
    workers.emplace_back([&]() {
      try {
        while (true) {
          const size_t pairIndex = nextIndex.fetch_add(1);
          if (pairIndex >= pairs.size()) {
            break;
          }
          static_cast<void>(getOuterNfp(pairs[pairIndex].A, pairs[pairIndex].B, false, config, cache));
        }
      } catch (...) {
        std::lock_guard lock(errorMutex);
        if (!firstError) {
          firstError = std::current_exception();
        }
      }
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  if (firstError) {
    std::rethrow_exception(firstError);
  }
}

}  // namespace

// Initialize the nesting coordinator and its cache.
BackgroundOrchestrator::BackgroundOrchestrator(NfpCache cache) : cache_(std::move(cache)) {}

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

  if (data.config.algorithm == NestingAlgorithm::Nfp) {
    if(data.config.timeLimitSeconds!=0 || std::any_of(parts.begin(),parts.end(),
        [](const Polygon& p) { return !p.allowedAngles.empty(); }))
      throw std::invalid_argument("Per-part angles and time limits require algorithm: bitmap");
    const auto tNfpStart = std::chrono::steady_clock::now();
    auto pairs = preprocessMissingPairs(parts, cache_);
    // Only independent cache warm-up runs in parallel; greedy placement stays sequential so
    // accepted placements and tie-breaking remain deterministic for the same input/order.
    precomputeMissingPairs(pairs, data.config, cache_, data.config.threads);
    const auto tNfpEnd = std::chrono::steady_clock::now();
    runStats.timings.nfpPrecomputeMs =
        std::chrono::duration<double, std::milli>(tNfpEnd - tNfpStart).count();
    sink.onProgress(data.index, 0.5);

    const auto tPlacementStart = std::chrono::steady_clock::now();
    runStats.placement =
        placeParts(sheets, parts, data.config, cache_, [&](double p) { sink.onProgress(data.index, p); });
    const auto tPlacementEnd = std::chrono::steady_clock::now();
    runStats.timings.placementMs =
        std::chrono::duration<double, std::milli>(tPlacementEnd - tPlacementStart).count();
    runStats.simdBackend = "n/a";
  } else {
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
  }

  sink.onResult(runStats.placement);
  runStats.timings.totalMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return runStats;
}

}  // namespace clinesting
