// Included inside bitmap_nesting.cpp's anonymous namespace to reuse the exact
// same raster representation as the ordinary placer and OpenCL kernel.

// One OpenCL context/rotation atlas, with FIFO admission for CPU search workers.
// A worker holds the device only while updating its bitmap and filtering a batch.
struct RecursiveGpuTiming {double waitMs=0,updateMs=0,dispatchMs=0;uint64_t uploadBytes=0;};
struct RecursiveGpuUpdate {size_t sheet;GpuCandidate candidate;};
class RecursiveGpuBroker {
  GpuBitmap device_;
  std::mutex mutex_;
  std::condition_variable ready_;
  uint64_t next_{0},serving_{0};
  bool atlasReady_{false};
  size_t sheet_{SIZE_MAX};
  struct Lease {
    RecursiveGpuBroker& broker;
    explicit Lease(RecursiveGpuBroker& b):broker(b) {
      std::unique_lock lock(b.mutex_);const auto ticket=b.next_++;
      b.ready_.wait(lock,[&]{return ticket==b.serving_;});
    }
    ~Lease() {{std::lock_guard lock(broker.mutex_);++broker.serving_;}broker.ready_.notify_all();}
  };
 public:
  explicit RecursiveGpuBroker(int device):device_(device) {}
  std::string name() const {return device_.device().name;}
  template<class Loader> void prepare(Loader&& loader) {
    Lease turn(*this);
    if(atlasReady_) return;
    loader(device_);atlasReady_=true;
  }
  std::vector<uint8_t> filter(size_t owner,size_t sheet,const BitmapGrid& material,
      const BitmapGrid& occupied,std::vector<RecursiveGpuUpdate>& updates,uint64_t first,
      uint32_t count,uint32_t rows,uint32_t step,uint32_t firstMask,uint32_t masks,uint32_t edge,
      RecursiveGpuTiming& timing) {
    const auto start=std::chrono::steady_clock::now();
    Lease turn(*this);
    const auto acquired=std::chrono::steady_clock::now();
    timing.waitMs+=std::chrono::duration<double,std::milli>(acquired-start).count();
    if(sheet_!=sheet) {
      device_.setSheet(material.widthPx,material.heightPx,material.bits);
      sheet_=sheet;
    }
    if(!device_.selectOccupancySlot(owner)) {
      device_.setOccupancy(occupied.bits);timing.uploadBytes+=occupied.bits.size()*sizeof(uint64_t);
    }
    else for(const auto& update:updates) if(update.sheet==sheet)
      device_.toggleMask(update.candidate.rotation,update.candidate.x,update.candidate.y);
    updates.clear();
    const auto updated=std::chrono::steady_clock::now();
    timing.updateMs+=std::chrono::duration<double,std::milli>(updated-acquired).count();
    auto flags=device_.filterGrid(first,count,rows,step,material.widthPx,material.heightPx,firstMask,masks,edge,edge);
    timing.dispatchMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-updated).count();
    return flags;
  }
};

PlacementResult recursiveBitmapSearch(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const Config& config,BitmapNestingStats* stats,
    const BitmapLayoutCallback& onLayout,std::shared_ptr<RecursiveGpuBroker> sharedGpu={},
    size_t worker=0,uint32_t rootFirst=0,uint32_t rootCount=0) {
  const auto started=std::chrono::steady_clock::now();
  SearchDeadline deadline(config.timeLimitSeconds,config.stopRequested);
  ParallelLoop parallel(size_t(normalizeWorkerCount(config.threads)));
  RasterPixels pixels(config.bitmapResolutionMm,deadline,parallel);
  BitmapNestingStats counts;
  counts.unplacedParts=parts.size();
  counts.totalStartedTrials=counts.startedTrials=1;
  counts.cpuWorkersUsed=parallel.size();
  PlacementResult best;best.unplaced=parts;
  size_t bestCount=0;
  double bestWaste=std::numeric_limits<double>::infinity(),bestBounds=bestWaste,bestSpread=bestWaste;
  struct Stock {
    Bounds bounds;
    BitmapGrid material,occupied;
    std::vector<Polygon> geometry;
    std::vector<Placement> placed;
  };
  std::vector<Stock> stocks;
  struct Policy {uint32_t first,count;int minWidth{INT32_MAX},minHeight{INT32_MAX};};
  std::vector<Policy> policies;
  std::vector<size_t> excluded;
  std::vector<std::unique_ptr<RasterMask>> masks;
  auto gpu=std::move(sharedGpu);
  std::vector<RecursiveGpuUpdate> gpuUpdates;
  RecursiveGpuTiming gpuTiming;
  auto gpuFailure=[&](const std::exception& error) {
    if(!config.gpuFallbackToCpu) throw std::runtime_error(error.what());
    counts.gpuFallbackReason=error.what();gpu.reset();
  };
  // XOR is reversible because a committed mask never overlaps occupied pixels.
  auto toggle=[&](size_t sheet,size_t rotation,int x,int y) {
    auto& stock=stocks[sheet];const auto& m=*masks[rotation];
    if(gpu) gpuUpdates.push_back({sheet,{x,y,uint32_t(rotation)}});
    pixels.ensure(m,false);
    const size_t shift=size_t(x)%64,word=size_t(x)/64;
    const size_t needed=(size_t(m.widthPx)+shift+63)/64;
    for(int row=0;row<m.heightPx;++row) for(size_t w=0;w<needed;++w) {
      const auto src=size_t(row)*m.wordsPerRow;
      uint64_t value=w<m.wordsPerRow ? m.bits[src+w] : 0;
      if(shift) value=(value<<shift)|(w && w-1<m.wordsPerRow ? m.bits[src+w-1]>>(64-shift) : 0);
      stock.occupied.bits[size_t(y+row)*stock.occupied.wordsPerRow+word+w]^=value;
    }
  };
  // Explicit stack preserves the recursive tree without exhausting the C++ call
  // stack. A frame retains its GPU flags while all child branches are explored.
  struct Frame {
    size_t sheet{0};uint64_t next{0},batchFirst{0};size_t cursor{0};
    uint32_t batchSize{4096};
    std::vector<uint8_t> flags;
    bool skipVisited{false},active{false},initialized{false};
    uint64_t chosenIndex{0};
    Candidate chosen{0,0,0};
  };
  std::vector<Frame> stack;
  std::vector<bool> placed(parts.size(),false);
  size_t placedCount=0;
  double placedArea=0;
  auto publish=[&] {
    if(placedCount<bestCount) return;
    double stockArea=0,boundsArea=0,spread=0;
    for(size_t s=0;s<stocks.size();++s) if(!stocks[s].placed.empty()) {
      stockArea+=polygonMaterialArea(sheets[s]);
      std::vector<Point> points;
      const auto sb=getPolygonBounds(sheets[s].points);
      for(const auto& p:stocks[s].geometry) {
        points.insert(points.end(),p.points.begin(),p.points.end());
        spread+=placementSpreadCost(p,sb);
      }
      const auto bounds=getPolygonBounds(points);boundsArea+=bounds.width*bounds.height;
    }
    const double waste=stockArea-placedArea;
    if(placedCount<bestCount || (placedCount==bestCount &&
        (waste>bestWaste+1e-8 || (std::abs(waste-bestWaste)<=1e-8 &&
        (boundsArea>bestBounds+1e-8 || (std::abs(boundsArea-bestBounds)<=1e-8 && spread>=bestSpread-1e-8)))))) return;
    bestCount=placedCount;bestWaste=waste;bestBounds=boundsArea;bestSpread=spread;
    best={};best.area=placedArea;best.totalarea=stockArea;
    for(size_t s=0;s<stocks.size();++s) if(!stocks[s].placed.empty())
      best.placements.push_back({sheets[s].source,sheets[s].id,stocks[s].placed});
    for(size_t i=0;i<parts.size();++i) if(!placed[i]) best.unplaced.push_back(parts[i]);
    best.fitness=double(best.unplaced.size());
    best.utilisation=stockArea>0 ? 100*placedArea/stockArea : 0;
    counts.occupiedBoundsArea=boundsArea;counts.placementSpreadCost=spread;counts.placedParts=placedCount;
    counts.unplacedParts=best.unplaced.size();counts.acceptedPlacements=placedCount;
    counts.totalBitmapMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    if(onLayout) onLayout(best,counts);
  };
  try {
    if(config.gpuEnabled) try {if(!gpu) gpu=std::make_shared<RecursiveGpuBroker>(config.gpuDevice);counts.gpuDevice=gpu->name();}
      catch(const std::exception& e) {gpuFailure(e);}
    stocks.resize(sheets.size());
    for(size_t s=0;s<sheets.size();++s) {
      stocks[s].material=rasterizeSheetMaterial(sheets[s],config.bitmapResolutionMm,stocks[s].bounds,deadline,parallel);
      stocks[s].occupied=makeBitmapGrid(stocks[s].material.widthPx,stocks[s].material.heightPx);
    }
    std::unordered_map<std::string,Policy> cached;

    for(const auto& part:parts) {
      deadline.check();
      const auto key=searchPolicyKey(part);
      auto found=cached.find(key);
      if(found!=cached.end()) {policies.push_back(found->second);continue;}
      Policy policy{uint32_t(masks.size()),uint32_t(part.allowedAngles.size())};
      for(double angle:part.allowedAngles) {
        deadline.check();
        masks.push_back(std::make_unique<RasterMask>(rasterizePartMask(part,angle,config.bitmapResolutionMm,config.curveTolerance)));
        const auto& m=*masks.back();
        policy.minWidth=std::min(policy.minWidth,m.widthPx);policy.minHeight=std::min(policy.minHeight,m.heightPx);

      }
      cached.emplace(key,policy);policies.push_back(policy);
    }
    if(gpu && !masks.empty()) try {
      gpu->prepare([&](GpuBitmap& device) {
        std::vector<GpuMaskInfo> atlas;std::vector<uint64_t> words;
        for(const auto& item:masks) {
          deadline.check();pixels.ensure(*item);const auto& m=*item;
          if(words.size()+m.bits.size()>64ULL*1024*1024)
            throw std::runtime_error("Recursive rotation atlas exceeds 512 MiB");
          atlas.push_back({uint32_t(m.widthPx),uint32_t(m.heightPx),uint32_t(m.wordsPerRow),uint32_t(words.size())});
          words.insert(words.end(),m.bits.begin(),m.bits.end());
        }
        device.setMasks(atlas,words);
      });
    } catch(const std::exception& e) {gpuFailure(e);}
    counts.cachedMaskCount=masks.size();
    excluded.resize(masks.size(),0);
    auto lastProgress=std::chrono::steady_clock::now();
    stack.emplace_back();
    while(!stack.empty()) {
      deadline.check();
      const size_t depth=stack.size()-1;
      const auto now=std::chrono::steady_clock::now();
      if(config.searchProgress && now-lastProgress>=std::chrono::seconds(5)) {
        std::ostringstream message;
        message<<"Recursive progress: worker="<<worker+1<<" rootAngles="<<rootFirst<<"+"<<rootCount<<" elapsed="<<std::chrono::duration_cast<std::chrono::seconds>(now-started).count()
          <<"s depth="<<depth<<" best="<<bestCount<<" nodes="<<counts.recursiveNodes
          <<" backtracks="<<counts.recursiveBacktracks<<" duplicateSkips="<<counts.recursiveDuplicateSkips
          <<" gpuBatches="<<counts.gpuBatches<<" gpuCandidates="<<counts.gpuCandidates
          <<" vectorChecks="<<counts.vectorChecks
          <<" gpuWaitMs="<<uint64_t(gpuTiming.waitMs)<<" gpuUpdateMs="<<uint64_t(gpuTiming.updateMs)
          <<" gpuDispatchMs="<<uint64_t(gpuTiming.dispatchMs)<<" gpuUploadBytes="<<gpuTiming.uploadBytes<<" exhausted=0";
        config.searchProgress(message.str());lastProgress=now;
      }
      counts.recursiveMaxDepth=std::max(counts.recursiveMaxDepth,depth);
      auto& frame=stack.back();
      if(frame.active) {
        auto& stock=stocks[frame.sheet];
        toggle(frame.sheet,frame.chosen.rotation,frame.chosen.x,frame.chosen.y);
        stock.placed.pop_back();stock.geometry.pop_back();
        placed[depth]=false;--placedCount;placedArea-=polygonMaterialArea(parts[depth]);
        frame.active=false;++counts.recursiveBacktracks;
      }
      // Count bound only: compactness branches with equal counts remain valid.
      if(depth==parts.size() || placedCount+parts.size()-depth<bestCount) {
        publish();stack.pop_back();continue;
      }
      if(!frame.initialized) {
        frame.initialized=true;
        // Once a copy is skipped, later interchangeable copies need not repeat
        // its search. Any such layout was already covered with the earlier ID.
        size_t available=0;
        for(size_t i=depth;i<parts.size();++i) if(!excluded[policies[i].first]) ++available;
        if(!available || placedCount+available<bestCount) {
          counts.recursiveDuplicateSkips+=parts.size()-depth-available;
          publish();stack.pop_back();continue;
        }
        if(excluded[policies[depth].first]) {
          ++counts.recursiveDuplicateSkips;
          frame.sheet=stocks.size();
        }
        // Identical geometry/angle policies are interchangeable. Canonical
        // sheet/grid order removes copy permutations without losing layouts.
        for(size_t previous=depth;frame.sheet<stocks.size() && previous>0;--previous) {
          const size_t i=previous-1;
          if(placed[i] && policies[i].first==policies[depth].first) {
            frame.sheet=stack[i].sheet;frame.next=stack[i].chosenIndex+1;break;
          }
        }
      }
      if(frame.sheet==stocks.size()) {
        if(depth==0 && rootFirst!=0) {stack.pop_back();continue;}
        if(!frame.skipVisited) {
          frame.skipVisited=true;++excluded[policies[depth].first];stack.emplace_back();
        } else {--excluded[policies[depth].first];stack.pop_back();}
        continue;
      }
      auto& stock=stocks[frame.sheet];auto policy=policies[depth];
      if(depth==0 && rootCount) {policy.first+=rootFirst;policy.count=rootCount;}
      const uint32_t step=uint32_t(config.bitmapSearchStepPx);
      // A stock-margin bound skips billions of obviously invalid border
      // candidates on fine rotation grids without changing the search lattice.
      const double edgePixels=std::ceil(config.sheetSpacing/config.bitmapResolutionMm/step)*step;
      if(edgePixels>=double(std::min(stock.material.widthPx,stock.material.heightPx))) {
        ++frame.sheet;frame.next=0;frame.flags.clear();frame.cursor=0;frame.batchSize=4096;continue;
      }
      const uint32_t edge=uint32_t(edgePixels);
      const int64_t spanX=int64_t(stock.material.widthPx)-2*edge-policy.minWidth;
      const int64_t spanY=int64_t(stock.material.heightPx)-2*edge-policy.minHeight;
      if(spanX<0||spanY<0) {++frame.sheet;frame.next=0;frame.flags.clear();frame.cursor=0;frame.batchSize=4096;continue;}
      const uint32_t rows=uint32_t(spanY/step+1);
      const uint64_t columns=uint64_t(spanX/step+1);
      const uint64_t total=columns*rows*policy.count;
      if(frame.cursor==frame.flags.size()) {
        if(frame.next==total) {++frame.sheet;frame.next=0;frame.flags.clear();frame.cursor=0;frame.batchSize=4096;continue;}
        frame.batchFirst=frame.next;
        // Near a fresh branch, a feasible position often appears immediately.
        // Avoid computing a huge unused suffix before descending. Fully consumed
        // ranges grow geometrically to the configured maximum on sparse sheets.
        const uint32_t limit=std::min<uint32_t>(config.gpuBatchSize,frame.batchSize);
        const uint32_t batch=uint32_t(std::min<uint64_t>(limit,total-frame.next));
        frame.batchSize=std::min<uint32_t>(config.gpuBatchSize,limit*2);
        if(gpu) try {
          frame.flags=gpu->filter(worker,frame.sheet,stock.material,stock.occupied,gpuUpdates,
              frame.next,batch,rows,step,policy.first,policy.count,edge,gpuTiming);
          ++counts.gpuBatches;counts.gpuCandidates+=batch;
        } catch(const std::exception& e) {gpuFailure(e);}
        if(!gpu) {
          frame.flags.assign(batch,0);
          for(uint32_t i=0;i<batch;++i) {
            if((i&255)==0) deadline.check();
            const uint64_t index=frame.next+i;const auto r=policy.first+index%policy.count;
            const int x=int(index/policy.count/rows)*int(step)+int(edge),y=int((index/policy.count)%rows)*int(step)+int(edge);
            pixels.ensure(*masks[r]);
            frame.flags[i]=maskFits(stock.occupied,stock.material,*masks[r],x,y,false);
          }
        }
        frame.next+=batch;frame.cursor=0;counts.candidatesExamined+=batch;
      }
      const auto nextValid=std::find(frame.flags.begin()+frame.cursor,frame.flags.end(),uint8_t(1));
      counts.bitmapCollisions+=size_t(nextValid-(frame.flags.begin()+frame.cursor));
      frame.cursor=size_t(nextValid-frame.flags.begin());
      if(frame.cursor==frame.flags.size()) continue;
      const size_t cursor=frame.cursor++;
      const uint64_t index=frame.batchFirst+cursor;
      const size_t r=policy.first+index%policy.count;
      const int x=int(index/policy.count/rows)*int(step)+int(edge),y=int((index/policy.count)%rows)*int(step)+int(edge);
      const auto& mask=*masks[r];
      const Point translation{stock.bounds.x+x*config.bitmapResolutionMm-mask.minX,
                              stock.bounds.y+y*config.bitmapResolutionMm-mask.minY,true};
      auto geometry=shiftPolygon(mask.rotatedPart,translation);
      if(violatesSheetClearance(geometry,sheets[frame.sheet],config)) {++counts.boundaryRejects;continue;}
      bool collision=false;
      for(const auto& previous:stock.geometry) {
        deadline.check();++counts.vectorChecks;
        if(violatesPartClearance(geometry,previous,config)) {collision=true;break;}
      }
      if(collision) {++counts.vectorValidationRejects;continue;}
      toggle(frame.sheet,r,x,y);
      const auto& part=parts[depth];
      stock.placed.push_back({translation.x,translation.y,part.id,mask.rotationDeg,part.source,part.filename,0,{}});
      stock.geometry.push_back(std::move(geometry));
      placed[depth]=true;++placedCount;placedArea+=polygonMaterialArea(part);
      frame.active=true;frame.chosen={x,y,r};frame.chosenIndex=(index/policy.count)*policies[depth].count+(r-policies[depth].first);++counts.recursiveNodes;
      publish();stack.emplace_back();
    }
    counts.recursiveExhausted=true;
  } catch(const SearchTimeExpired&) {}
  counts.cancelled=deadline.cancelled();counts.timeLimitReached=deadline.timedOut();
  counts.completedTrials=counts.recursiveExhausted ? 1 : 0;
  counts.totalBitmapMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  if(stats) *stats=counts;
  return best;
}

// Disjoint root angle ranges cover the same finite tree. Only worker zero owns
// the root-skip branch; deeper parts always retain their full allowed angle list.
PlacementResult parallelRecursiveSearch(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const Config& config,BitmapNestingStats* stats,
    const BitmapLayoutCallback& onLayout) {
  const size_t angles=parts.empty() ? 1 : parts.front().allowedAngles.size();
  const size_t workers=std::min<size_t>(normalizeWorkerCount(config.threads),angles);
  if(workers<=1) return recursiveBitmapSearch(sheets,parts,config,stats,onLayout);
  SearchDeadline deadline(config.timeLimitSeconds,config.stopRequested);
  std::shared_ptr<RecursiveGpuBroker> gpu;
  std::string fallback;
  if(config.gpuEnabled && !deadline.expired()) try {gpu=std::make_shared<RecursiveGpuBroker>(config.gpuDevice);}
    catch(const std::exception& e) {if(!config.gpuFallbackToCpu) throw;fallback=e.what();}
  std::mutex publishMutex;
  std::atomic<bool> failed{false};std::exception_ptr error;
  PlacementResult best;best.unplaced=parts;BitmapNestingStats bestStats;
  bool haveBest=false;
  auto consider=[&](const PlacementResult& result,const BitmapNestingStats& data) {
    std::lock_guard lock(publishMutex);
    if(haveBest && !improvesLayout(layoutQuality(sheets,result,data),layoutQuality(sheets,best,bestStats))) return;
    best=result;bestStats=data;haveBest=true;
    bestStats.cpuWorkersUsed=workers;bestStats.workersUsed=workers;
    if(onLayout) onLayout(best,bestStats);
  };
  std::vector<BitmapNestingStats> results(workers);
  const auto started=std::chrono::steady_clock::now();
  std::vector<std::jthread> pool;
  for(size_t worker=0;worker<workers;++worker) pool.emplace_back([&,worker] {
    try {
      auto cfg=config;cfg.threads=1;cfg.gpuEnabled=bool(gpu);
      cfg.timeLimitSeconds=0;
      // Avoid serializing every pairwise geometry check on the shared stop mutex.
      cfg.stopRequested=[&,nextPoll=std::chrono::steady_clock::time_point{}]() mutable {
        if(failed.load()) return true;
        const auto now=std::chrono::steady_clock::now();
        if(now<nextPoll) return false;
        nextPoll=now+std::chrono::milliseconds(20);return deadline.expired();
      };
      cfg.searchProgress=[&,worker](const std::string& message) {
        if(!config.searchProgress) return;
        std::lock_guard lock(publishMutex);config.searchProgress(message);
      };
      const auto result=recursiveBitmapSearch(sheets,parts,cfg,&results[worker],consider,gpu,
          worker,uint32_t(angles*worker/workers),uint32_t(angles*(worker+1)/workers-angles*worker/workers));
      consider(result,results[worker]);
    } catch(...) {
      failed=true;std::lock_guard lock(publishMutex);if(!error) error=std::current_exception();
    }
  });
  pool.clear();
  if(error) std::rethrow_exception(error);
  BitmapNestingStats total=bestStats;
  total.recursiveNodes=total.recursiveBacktracks=total.recursiveDuplicateSkips=0;
  total.gpuBatches=total.gpuCandidates=total.vectorChecks=total.candidatesExamined=0;
  total.recursiveExhausted=true;total.completedTrials=0;
  for(const auto& r:results) {
    total.recursiveNodes+=r.recursiveNodes;total.recursiveBacktracks+=r.recursiveBacktracks;
    total.recursiveDuplicateSkips+=r.recursiveDuplicateSkips;
    total.recursiveMaxDepth=std::max(total.recursiveMaxDepth,r.recursiveMaxDepth);
    total.recursiveExhausted&=r.recursiveExhausted;total.completedTrials+=r.completedTrials;
    total.gpuBatches+=r.gpuBatches;total.gpuCandidates+=r.gpuCandidates;
    total.vectorChecks+=r.vectorChecks;total.candidatesExamined+=r.candidatesExamined;
    if(!r.gpuDevice.empty()) total.gpuDevice=r.gpuDevice;
    if(!r.gpuFallbackReason.empty()) fallback=r.gpuFallbackReason;
  }
  total.gpuFallbackReason=fallback;
  total.cpuWorkersUsed=total.workersUsed=workers;
  total.totalStartedTrials=total.startedTrials=workers;
  total.cancelled=deadline.cancelled();total.timeLimitReached=deadline.timedOut();
  total.totalBitmapMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  if(stats) *stats=total;
  return best;
}
