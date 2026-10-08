#include "clinesting/continuous_nesting.hpp"
#include "clinesting/geometry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <stdexcept>

namespace clinesting {
// Compute unplaced count and unused stock/bounding areas.
LayoutQuality layoutQuality(const std::vector<Polygon>& sheets,const PlacementResult& result,
                            const BitmapNestingStats& stats) {
  double usedArea=0;
  for(const auto& placed:result.placements) if(!placed.sheetplacements.empty()) {
    const auto sheet=std::find_if(sheets.begin(),sheets.end(),[&](const Polygon& p) { return p.id==placed.sheetid; });
    if(sheet==sheets.end()) throw std::invalid_argument("Unknown sheet in layout quality calculation");
    usedArea+=polygonMaterialArea(*sheet);
  }
  return {result.unplaced.size(),std::max(0.0,usedArea-result.area),
          std::max(0.0,stats.occupiedBoundsArea-result.area),stats.placementSpreadCost};
}
// Compare layouts by completeness, stock waste and compactness.
bool improvesLayout(const LayoutQuality& candidate,const LayoutQuality& incumbent) {
  if(candidate.unplaced!=incumbent.unplaced) return candidate.unplaced<incumbent.unplaced;
  const auto compare=[](double a,double b) {
    const double tolerance=std::max({1.0,std::abs(a),std::abs(b)})*1e-9;
    return a<b-tolerance ? -1 : a>b+tolerance ? 1 : 0;
  };
  const int waste=compare(candidate.usedSheetWasteArea,incumbent.usedSheetWasteArea);
  if(waste!=0) return waste<0;
  const int compact=compare(candidate.compactWasteArea,incumbent.compactWasteArea);
  return compact<0 || (compact==0 && compare(candidate.placementSpreadCost,incumbent.placementSpreadCost)<0);
}

// Coordinate timed restarts or systematic continuous rounds; retain improvements.
static OrchestratorRunStats optimize(BackgroundRequest request,const std::function<bool()>& stop,
                                     const ImprovementCallback& onImprovement,bool timed) {
  if(!stop || !onImprovement)
    throw std::invalid_argument("Continuous search needs a stop predicate and a result callback");
  if(!std::isfinite(request.config.continuousRoundSeconds) || request.config.continuousRoundSeconds<0.01 ||
      request.config.continuousRoundSeconds>86400)
    throw std::invalid_argument("continuousRoundSeconds must be between 0.01 and 86400");
  std::mutex stopMutex,incumbentMutex,progressMutex;
  const auto stopped=[&] {std::lock_guard lock(stopMutex);return stop();};
  const int configuredWorkers=request.config.threads>0 ? request.config.threads : int(std::max(1u,std::thread::hardware_concurrency()));
  const bool hybrid=!timed && configuredWorkers>=2 && request.individual.placement.size()>=16;
  auto originalProgress=request.config.searchProgress;
  request.config.searchProgress=[&](const std::string& message) {
    if(originalProgress) {std::lock_guard lock(progressMutex);originalProgress(message);}
  };
  const double budget=request.config.timeLimitSeconds;
  if(timed && (!std::isfinite(budget) || budget<=0 || budget>86400))
    throw std::invalid_argument("timed mode requires timeLimitSeconds in (0,86400]");
  request.config.continuous=!timed;
  const bool recursive=!timed;
  request.config.mode=timed ? SearchMode::Timed : SearchMode::Continuous;
  request.config.searchIteration=0;
  std::optional<LayoutQuality> incumbent;
  size_t sequence=0;
  const auto started=std::chrono::steady_clock::now();
  const auto end=started+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(timed ? budget : 0));
  auto expired=[&] { return timed && std::chrono::steady_clock::now()>=end; };
  request.config.stopRequested=[&] { return stopped() || expired(); };
  OrchestratorRunStats best;
  best.placement.unplaced=request.individual.placement;
  best.bitmapStats.unplacedParts=request.individual.placement.size();
  size_t gpuBatches=0,gpuCandidates=0,totalStartedTrials=0,cachePeak=0;
  size_t cpuWorkers=0,candidateWorkers=0;
  size_t recursiveNodes=0,recursiveBacktracks=0,recursiveMaxDepth=0,duplicateSkips=0;
  bool recursiveExhausted=false;size_t contactTrials=0,groupTrials=0,localRepairTrials=0,maxPatternGroupSize=0;
  size_t maxInterlockingCapacity=0;
  auto recordWorkers=[&](const BitmapNestingStats& stats) {
    maxPatternGroupSize=std::max(maxPatternGroupSize,stats.maxPatternGroupSize);
    maxInterlockingCapacity=std::max(maxInterlockingCapacity,stats.maxInterlockingCapacity);
    contactTrials+=stats.contactTrials;groupTrials+=stats.groupTrials;localRepairTrials+=stats.localRepairTrials;
    duplicateSkips+=stats.recursiveDuplicateSkips;
    recursiveNodes+=stats.recursiveNodes;recursiveBacktracks+=stats.recursiveBacktracks;
    recursiveMaxDepth=std::max(recursiveMaxDepth,stats.recursiveMaxDepth);recursiveExhausted|=stats.recursiveExhausted;
    totalStartedTrials+=stats.totalStartedTrials;
    cachePeak=std::max(cachePeak,stats.rejectionCachePeakBytes);
    cpuWorkers=std::max(cpuWorkers,stats.cpuWorkersUsed);
    candidateWorkers=std::max(candidateWorkers,stats.candidateWorkersUsed);
  };
  std::string gpuDevice,gpuFallback;
  auto consider=[&](const PlacementResult& placement,const BitmapNestingStats& stats) {
    std::lock_guard lock(incumbentMutex);
    const auto quality=layoutQuality(request.sheets,placement,stats);
    if(incumbent && !improvesLayout(quality,*incumbent)) return;
    OrchestratorRunStats result;
    result.searchIteration=request.config.searchIteration;
    result.placement=placement;
    result.bitmapStats=stats;
    if(hybrid) {result.bitmapStats.continuousPortfolio=true;result.bitmapStats.cpuWorkersUsed=configuredWorkers;}
    result.timings.bitmapMs=stats.totalBitmapMs;
    result.timings.placementMs=stats.totalBitmapMs;
    result.timings.totalMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    onImprovement(request,result,sequence+1);
    best=result;
    incumbent=quality;
    ++sequence;
  };
  // Adapt nesting events to the current command-line workflow.
  class Sink final:public EventSink {
   public:
    // Receive the start-of-job notification and input counts.
    void onTestStart(const std::vector<Polygon>&,const std::vector<Polygon>&,const Config&,int) override {}
    // Receive the current nesting progress notification.
    void onProgress(int,double) override {}
    // Receive the completed placement result.
    void onResult(const PlacementResult&) override {}
  } sink;
  BackgroundOrchestrator orchestrator;
  // Seed a fast layout, reserving most of a timed job for competing strategies.
  // Short budgets still use the same global cancellation hook for GPU setup.
  if(!stopped() && !expired()) {
    auto seed=request;
    seed.config.mode=SearchMode::First;
    seed.config.bitmapTrials=1;
    seed.config.timeLimitSeconds=timed ? std::max(0.000001,std::min({budget*0.2,
        request.config.continuousRoundSeconds,std::chrono::duration<double>(end-std::chrono::steady_clock::now()).count()})) : request.config.continuousRoundSeconds;
    const auto initial=orchestrator.runWithStats(seed,sink,consider);
    recordWorkers(initial.bitmapStats);
    gpuBatches+=initial.bitmapStats.gpuBatches;
    gpuCandidates+=initial.bitmapStats.gpuCandidates;
    gpuDevice=initial.bitmapStats.gpuDevice;
    gpuFallback=initial.bitmapStats.gpuFallbackReason;
    consider(initial.placement,initial.bitmapStats);
    request.config.searchIteration=1;
  }
  while(!stopped() && !expired()) {
    request.config.timeLimitSeconds=timed ? std::min(request.config.continuousRoundSeconds,
        std::chrono::duration<double>(end-std::chrono::steady_clock::now()).count()) : 0;
    if(recursive) request.config.timeLimitSeconds=0;
    if(!recursive && request.config.timeLimitSeconds<=0) break;
    const auto result=[&] {
      if(!hybrid) return orchestrator.runWithStats(request,sink,consider);
      // Keep the persistent raster tree while independent CPU contact searches
      // revisit early placement decisions within short, bounded trials.
      const int gridWorkers=std::min(2,configuredWorkers-1),helpers=configuredWorkers-gridWorkers;
      std::atomic<bool> finished{false};std::atomic<uint64_t> nextTrial{0};
      std::mutex errorMutex;std::exception_ptr error;
      std::vector<BitmapNestingStats> helperStats(size_t(helpers),BitmapNestingStats{});
      std::vector<std::jthread> pool;
      for(int worker=0;worker<helpers;++worker) pool.emplace_back([&,worker](std::stop_token token) {
        try {
          while(!token.stop_requested() && !finished && !stopped()) {
            const uint64_t trial=nextTrial.fetch_add(1);
            const uint64_t variation=trial/4;
            auto cfg=request.config;cfg.threads=1;cfg.gpuEnabled=false;cfg.bitmapTrials=1;
            cfg.mode=SearchMode::First;cfg.bitmapNfpContacts=true;cfg.searchIteration=variation+1;cfg.bitmapContactColumnFirst=trial%4==2;
            cfg.bitmapGroupTrial=trial%4==0;cfg.bitmapBottomLeft=!cfg.bitmapGroupTrial;
            cfg.timeLimitSeconds=cfg.bitmapGroupTrial ? 5.0 : 3.0;
            cfg.stopRequested=[&,nextPoll=std::chrono::steady_clock::time_point{}]() mutable {
              if(token.stop_requested() || finished.load()) return true;
              const auto now=std::chrono::steady_clock::now();if(now<nextPoll) return false;
              nextPoll=now+std::chrono::milliseconds(20);return stopped();
            };
            PlacementResult candidate;BitmapNestingStats data;
            const bool repair=trial%4==3;
            if(repair) {
              PlacementResult incumbentCopy;
              {std::lock_guard lock(incumbentMutex);incumbentCopy=best.placement;}
              candidate=refillBitmapLayout(request.sheets,request.individual.placement,incumbentCopy,cfg,&data);
              ++helperStats[size_t(worker)].localRepairTrials;data.localRepairTrials=1;
            } else {
              auto parts=request.individual.placement;
              // Bounded angular samples shift between trials; no forbidden angle
              // is synthesized, and the grid workers retain the full angle list.
              for(auto& part:parts) if(part.allowedAngles.size()>12) {
                const auto allowed=part.allowedAngles;part.allowedAngles.clear();
                for(size_t i=0;i<12;++i) part.allowedAngles.push_back(allowed[(i*allowed.size()/12+variation)%allowed.size()]);
              }
              std::mt19937_64 random(trial+1);std::shuffle(parts.begin(),parts.end(),random);
              candidate=placePartsBitmap(request.sheets,parts,cfg,&data,{},[&](const auto& layout,const auto& counters) {
                auto tagged=counters;
                if(cfg.bitmapGroupTrial) tagged.groupTrials=1;else tagged.contactTrials=1;
                consider(layout,tagged);
              });
              if(cfg.bitmapGroupTrial) {++helperStats[size_t(worker)].groupTrials;data.groupTrials=1;}
              else {++helperStats[size_t(worker)].contactTrials;data.contactTrials=1;}
            }
            helperStats[size_t(worker)].maxPatternGroupSize=std::max(helperStats[size_t(worker)].maxPatternGroupSize,data.maxPatternGroupSize);
            helperStats[size_t(worker)].maxInterlockingCapacity=std::max(helperStats[size_t(worker)].maxInterlockingCapacity,data.maxInterlockingCapacity);
            consider(candidate,data);
            request.config.searchProgress("Contact trial: worker="+std::to_string(worker+1)+" trial="+std::to_string(trial)+
              " strategy="+(repair?"repair":cfg.bitmapGroupTrial?"group":"nfp")+" placed="+
              std::to_string(request.individual.placement.size()-candidate.unplaced.size())+
              " groupSize="+std::to_string(data.maxPatternGroupSize)+" interlockingCapacity="+std::to_string(data.maxInterlockingCapacity)+
              " elapsedMs="+std::to_string(uint64_t(data.totalBitmapMs)));
          }
        } catch(...) {finished=true;std::lock_guard lock(errorMutex);if(!error) error=std::current_exception();}
      });
      auto grid=request;grid.config.threads=gridWorkers;
      grid.config.stopRequested=[&]{return finished.load() || stopped();};
      OrchestratorRunStats result;
      try {result=orchestrator.runWithStats(grid,sink,consider);}
      catch(...) {finished=true;pool.clear();throw;}
      finished=true;pool.clear();
      if(error) std::rethrow_exception(error);
      for(const auto& s:helperStats) {
        result.bitmapStats.maxPatternGroupSize=std::max(result.bitmapStats.maxPatternGroupSize,s.maxPatternGroupSize);
        result.bitmapStats.maxInterlockingCapacity=std::max(result.bitmapStats.maxInterlockingCapacity,s.maxInterlockingCapacity);
        result.bitmapStats.contactTrials+=s.contactTrials;result.bitmapStats.groupTrials+=s.groupTrials;
        result.bitmapStats.localRepairTrials+=s.localRepairTrials;
      }
      result.bitmapStats.continuousPortfolio=true;result.bitmapStats.cpuWorkersUsed=configuredWorkers;
      // Contact/beam trials are heuristic; never label this combined search as exhaustive.
      result.bitmapStats.recursiveExhausted=false;
      return result;
    }();
    recordWorkers(result.bitmapStats);
    gpuBatches+=result.bitmapStats.gpuBatches;
    gpuCandidates+=result.bitmapStats.gpuCandidates;
    if(!result.bitmapStats.gpuDevice.empty()) gpuDevice=result.bitmapStats.gpuDevice;
    if(!result.bitmapStats.gpuFallbackReason.empty()) gpuFallback=result.bitmapStats.gpuFallbackReason;
    // Also handles a deadline reached before the first strategy started.
    consider(result.placement,result.bitmapStats);
    ++request.config.searchIteration;
    if(recursive) break;
  }
  best.bitmapStats.maxPatternGroupSize=maxPatternGroupSize;
  best.bitmapStats.maxInterlockingCapacity=maxInterlockingCapacity;
  best.bitmapStats.continuousPortfolio=hybrid;best.bitmapStats.contactTrials=contactTrials;
  best.bitmapStats.groupTrials=groupTrials;best.bitmapStats.localRepairTrials=localRepairTrials;
  best.bitmapStats.recursiveDuplicateSkips=duplicateSkips;
  best.bitmapStats.recursiveNodes=recursiveNodes;best.bitmapStats.recursiveBacktracks=recursiveBacktracks;
  best.bitmapStats.recursiveMaxDepth=recursiveMaxDepth;best.bitmapStats.recursiveExhausted=recursiveExhausted;
  best.bitmapStats.searchIterations=request.config.searchIteration;
  best.bitmapStats.totalStartedTrials=totalStartedTrials;
  best.bitmapStats.rejectionCachePeakBytes=cachePeak;
  best.bitmapStats.cpuWorkersUsed=cpuWorkers;
  best.bitmapStats.candidateWorkersUsed=candidateWorkers;
  best.bitmapStats.cancelled=stopped();
  best.bitmapStats.gpuBatches=gpuBatches;
  best.bitmapStats.gpuCandidates=gpuCandidates;
  best.bitmapStats.gpuDevice=std::move(gpuDevice);
  best.bitmapStats.gpuFallbackReason=std::move(gpuFallback);
  best.bitmapStats.timeLimitReached=expired() && !best.bitmapStats.cancelled;
  best.timings.totalMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  return best;
}
// Restart optimization until stopped and publish strict improvements.
OrchestratorRunStats runContinuousNesting(BackgroundRequest request,const std::function<bool()>& stop,
                          const ImprovementCallback& onImprovement) {
  return optimize(std::move(request),stop,onImprovement,false);
}
// Optimize until the shared deadline and return the best candidate.
OrchestratorRunStats runTimedNesting(BackgroundRequest request,const std::function<bool()>& stop) {
  return optimize(std::move(request),stop,[](const auto&,const auto&,size_t) {},true);
}
}
