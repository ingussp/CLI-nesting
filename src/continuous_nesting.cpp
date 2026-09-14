#include "deepnestcpp/continuous_nesting.hpp"
#include "deepnestcpp/geometry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace deepnest {
LayoutQuality layoutQuality(const std::vector<Polygon>& sheets,const PlacementResult& result,
                            const BitmapNestingStats& stats) {
  double usedArea=0;
  for(const auto& placed:result.placements) if(!placed.sheetplacements.empty()) {
    const auto sheet=std::find_if(sheets.begin(),sheets.end(),[&](const Polygon& p) { return p.id==placed.sheetid; });
    if(sheet==sheets.end()) throw std::invalid_argument("Unknown sheet in layout quality calculation");
    usedArea+=polygonMaterialArea(*sheet);
  }
  return {result.unplaced.size(),std::max(0.0,usedArea-result.area),
          std::max(0.0,stats.occupiedBoundsArea-result.area)};
}
bool improvesLayout(const LayoutQuality& candidate,const LayoutQuality& incumbent) {
  if(candidate.unplaced!=incumbent.unplaced) return candidate.unplaced<incumbent.unplaced;
  const auto compare=[](double a,double b) {
    const double tolerance=std::max({1.0,std::abs(a),std::abs(b)})*1e-9;
    return a<b-tolerance ? -1 : a>b+tolerance ? 1 : 0;
  };
  const int waste=compare(candidate.usedSheetWasteArea,incumbent.usedSheetWasteArea);
  return waste<0 || (waste==0 && compare(candidate.compactWasteArea,incumbent.compactWasteArea)<0);
}

static OrchestratorRunStats optimize(BackgroundRequest request,const std::function<bool()>& stop,
                                     const ImprovementCallback& onImprovement,bool timed) {
  if(request.config.algorithm!=NestingAlgorithm::Bitmap || !stop || !onImprovement)
    throw std::invalid_argument("Continuous search needs bitmap mode, a stop predicate and a result callback");
  if(!std::isfinite(request.config.continuousRoundSeconds) || request.config.continuousRoundSeconds<0.01 ||
      request.config.continuousRoundSeconds>86400)
    throw std::invalid_argument("continuousRoundSeconds must be between 0.01 and 86400");
  const double budget=request.config.timeLimitSeconds;
  if(timed && (!std::isfinite(budget) || budget<=0 || budget>86400))
    throw std::invalid_argument("timed mode requires timeLimitSeconds in (0,86400]");
  request.config.continuous=!timed;
  request.config.mode=timed ? SearchMode::Timed : SearchMode::Continuous;
  request.config.searchIteration=0;
  std::optional<LayoutQuality> incumbent;
  size_t sequence=0;
  const auto started=std::chrono::steady_clock::now();
  const auto end=started+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timed ? budget : 0));
  auto expired=[&] { return timed && std::chrono::steady_clock::now()>=end; };
  request.config.stopRequested=[&] { return stop() || expired(); };
  OrchestratorRunStats best;
  best.placement.unplaced=request.individual.placement;
  best.bitmapStats.unplacedParts=request.individual.placement.size();
  size_t gpuBatches=0,gpuCandidates=0;
  std::string gpuDevice,gpuFallback;
  auto consider=[&](const PlacementResult& placement,const BitmapNestingStats& stats) {
    const auto quality=layoutQuality(request.sheets,placement,stats);
    if(incumbent && !improvesLayout(quality,*incumbent)) return;
    OrchestratorRunStats result;
    result.searchIteration=request.config.searchIteration;
    result.placement=placement;
    result.bitmapStats=stats;
    result.timings.bitmapMs=stats.totalBitmapMs;
    result.timings.placementMs=stats.totalBitmapMs;
    result.timings.totalMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    onImprovement(request,result,sequence+1);
    if(timed) best=result;
    incumbent=quality;
    ++sequence;
  };
  class Sink final:public EventSink {
   public:
    void onTestStart(const std::vector<Polygon>&,const std::vector<Polygon>&,const Config&,int) override {}
    void onProgress(int,double) override {}
    void onResult(const PlacementResult&) override {}
  } sink;
  BackgroundOrchestrator orchestrator;
  while(!stop() && !expired()) {
    request.config.timeLimitSeconds=timed ? std::min(request.config.continuousRoundSeconds,
        std::chrono::duration<double>(end-std::chrono::steady_clock::now()).count()) : request.config.continuousRoundSeconds;
    if(request.config.timeLimitSeconds<=0) break;
    const auto result=orchestrator.runWithStats(request,sink,consider);
    gpuBatches+=result.bitmapStats.gpuBatches;
    gpuCandidates+=result.bitmapStats.gpuCandidates;
    if(!result.bitmapStats.gpuDevice.empty()) gpuDevice=result.bitmapStats.gpuDevice;
    if(!result.bitmapStats.gpuFallbackReason.empty()) gpuFallback=result.bitmapStats.gpuFallbackReason;
    // Also handles a deadline reached before the first strategy started.
    consider(result.placement,result.bitmapStats);
    ++request.config.searchIteration;
  }
  best.bitmapStats.cancelled=stop();
  best.bitmapStats.gpuBatches=gpuBatches;
  best.bitmapStats.gpuCandidates=gpuCandidates;
  best.bitmapStats.gpuDevice=std::move(gpuDevice);
  best.bitmapStats.gpuFallbackReason=std::move(gpuFallback);
  best.bitmapStats.timeLimitReached=expired() && !best.bitmapStats.cancelled;
  best.timings.totalMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  return best;
}
void runContinuousNesting(BackgroundRequest request,const std::function<bool()>& stop,
                          const ImprovementCallback& onImprovement) {
  static_cast<void>(optimize(std::move(request),stop,onImprovement,false));
}
OrchestratorRunStats runTimedNesting(BackgroundRequest request,const std::function<bool()>& stop) {
  return optimize(std::move(request),stop,[](const auto&,const auto&,size_t) {},true);
}
}
