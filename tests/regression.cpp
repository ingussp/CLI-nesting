#include "clinesting/bitmap_nesting.hpp"
#include "clinesting/geometry.hpp"
#include "clinesting/gpu_bitmap.hpp"
#include "clinesting/json_io.hpp"
#include "clinesting/continuous_nesting.hpp"
#include "raster_scanline.hpp"
#include "free_rectangles.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>

using namespace clinesting;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
Polygon rectangle(double x,double y,double w,double h,int id=1) {
  Polygon p; p.points={{x,y,true},{x+w,y,true},{x+w,y+h,true},{x,y+h,true}};
  p.id=id; p.source=std::to_string(id); p.allowedAngles={0}; return p;
}
Config settings() {
  Config c;c.mode=SearchMode::First;c.bitmapTrials=1;c.threads=1;
  c.spacing=3;c.sheetSpacing=2;c.holeSpacing=3;return c;
}
std::vector<Polygon> transformed(const std::vector<Polygon>& parts,const PlacementResult& result) {
  std::vector<Polygon> out;
  for(const auto& sheet:result.placements) for(const auto& placement:sheet.sheetplacements) {
    auto p=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.id==placement.id;});
    require(p!=parts.end(),"Unknown placement id");
    out.push_back(shiftPolygon(rotatePolygon(*p,placement.rotation),{placement.x,placement.y,true}));
  }
  return out;
}
void sameLayout(const PlacementResult& a,const PlacementResult& b) {
  require(a.placements.size()==b.placements.size(),"Hardware settings changed the number of sheets");
  require(a.unplaced.size()==b.unplaced.size(),"Hardware settings changed the number of unplaced parts");
  for(size_t i=0;i<a.unplaced.size();++i)
    require(a.unplaced[i].id==b.unplaced[i].id,"Hardware settings changed an unplaced part");
  for(size_t s=0;s<a.placements.size();++s) {
    const auto& ap=a.placements[s].sheetplacements;
    const auto& bp=b.placements[s].sheetplacements;
    require(ap.size()==bp.size(),"Hardware settings changed a sheet's part count");
    for(size_t i=0;i<ap.size();++i)
      require(ap[i].id==bp[i].id && ap[i].x==bp[i].x && ap[i].y==bp[i].y &&
              ap[i].rotation==bp[i].rotation,"Hardware settings changed deterministic placement");
  }
}
// Deliberately exhaustive reference for clearance; no bounding-box early exit.
double pointSegment(const Point& p,const Point& a,const Point& b) {
  const double dx=b.x-a.x,dy=b.y-a.y,l=dx*dx+dy*dy;
  const double t=l ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/l,0.0,1.0) : 0;
  return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
}
double distance(const Polygon& a,const Polygon& b) {
  double d=1e100;
  for(size_t i=0;i<a.points.size();++i) for(size_t j=0;j<b.points.size();++j) {
    d=std::min(d,pointSegment(a.points[i],b.points[j],b.points[(j+1)%b.points.size()]));
    d=std::min(d,pointSegment(b.points[j],a.points[i],a.points[(i+1)%a.points.size()]));
  }
  return d;
}
void valid(const Polygon& sheet,const std::vector<Polygon>& parts,const PlacementResult& result,const Config& cfg) {
  const auto placed=transformed(parts,result);
  for(size_t i=0;i<placed.size();++i) {
    const auto& p=placed[i];
    require(!hasMaterialOutsideSheet(p,sheet,cfg),"Outside stock or in stock cutout");
    require(distance(p,sheet)+1e-6>=cfg.sheetSpacing,"Sheet clearance");
    for(const auto& h:sheet.children) require(distance(p,h)+1e-6>=cfg.holeSpacing,"Stock hole clearance");
    for(size_t j=0;j<i;++j) {
      const auto& q=placed[j];
      require(!hasMaterialOverlap(p,q,cfg),"Overlapping material");
      require(distance(p,q)+1e-6>=cfg.spacing,"Part clearance");
      for(const auto& h:p.children) {
        require(distance(h,q)+1e-6>=cfg.holeSpacing,"Part hole clearance");
        for(const auto& k:q.children) require(distance(h,k)+1e-6>=cfg.holeSpacing,"Hole/hole clearance");
      }
      for(const auto& h:q.children) require(distance(p,h)+1e-6>=cfg.holeSpacing,"Insert clearance");
    }
  }
}
bool ray(const std::vector<Point>& p,double x,double y) {
  bool inside=false;
  for(size_t i=0,j=p.size()-1;i<p.size();j=i++)
    if((p[i].y>y)!=(p[j].y>y) && x<(p[j].x-p[i].x)*(y-p[i].y)/(p[j].y-p[i].y)+p[i].x) inside=!inside;
  return inside;
}
void scanlines() {
  std::mt19937 rng(13);std::vector<double> crossings;
  for(int trial=0;trial<80;++trial) {
    std::vector<Point> polygon;
    for(int i=0;i<12;++i) {
      const double angle=2*std::numbers::pi*i/12,r=5+(rng()%150)/10.0;
      polygon.push_back({-4+r*std::cos(angle),3+r*std::sin(angle),true});
    }
    if(trial%2) std::reverse(polygon.begin(),polygon.end());
    if(trial==0) polygon=rectangle(-4.5,-2.5,17,13).points;
    for(double resolution:{0.5,1.0,2.0}) for(int row=0;row<64;++row) {
      const double y=-30+(row+0.5)*resolution;std::vector<bool> bits(64,false);
      detail::rasterContourRow(polygon,y,-30,resolution,64,crossings,[&](int a,int b) {
        for(int x=a;x<b;++x) bits[x]=true;
      });
      for(int x=0;x<64;++x) require(bits[x]==ray(polygon,-30+(x+0.5)*resolution,y),"Scanline differs from pixel-center reference");
    }
  }
}
void duplicateContours() {
  auto parsed=parseNestingJson(R"({"sheets":[{"width":100,"height":100}],"parts":[{"points":[[0,0],[30,0],[30,30],[0,30],[0,0],[30,0],[30,30],[0,30]],"holes":[[[5,5],[25,5],[25,25],[5,25],[5,5],[25,5],[25,25],[5,25]]]}]})");
  const auto& p=parsed.individual.placement.front();
  require(p.points.size()==4 && p.children.front().points.size()==4,"Repeated contour not normalized");
  require(std::abs(polygonMaterialArea(p)-500)<1e-9,"Repeated contour area");
}
void holesFirst() {
  auto cfg=settings();auto sheet=rectangle(0,0,44,44,99);
  auto host=rectangle(0,0,40,40,2);host.children={rectangle(10,10,20,20)};
  std::vector<Polygon> parts={rectangle(0,0,10,10,1),host};
  BitmapNestingStats stats;auto result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty(),"Host must be placed before its insert");
  require(stats.holePlacements==1,"Insert must use the hole search");
  auto placed=transformed(parts,result);
  require(placed.front().id==host.id,"Host was not placed first");
  require(!hasMaterialOutsideSheet(placed[1],placed[0].children[0],cfg),"Insert not contained by hole");
  valid(sheet,parts,result,cfg);
  parts[0]=rectangle(0,0,18,18,1);
  result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.size()==1 && stats.holePlacements==0,"Oversized insert accepted");
  valid(sheet,parts,result,cfg);
}
void rotatedAndMultipleHoles() {
  auto cfg=settings();auto sheet=rectangle(100,-50,44,84,99);
  auto host=rectangle(0,0,80,40,20);host.allowedAngles={90};
  host.children={rectangle(5,5,30,30),rectangle(45,5,30,30)};
  std::vector<Polygon> parts={rectangle(0,0,15,15,1),rectangle(0,0,15,15,2),host};
  BitmapNestingStats stats;auto result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty() && stats.holePlacements==2,"Rotated multi-hole host");
  valid(sheet,parts,result,cfg);
}
void concavePocket() {
  auto cfg=settings();auto sheet=rectangle(0,0,54,54,99);
  auto host=rectangle(0,0,50,50,2);
  Polygon hole;hole.points={{5,5,true},{45,5,true},{45,15,true},{15,15,true},{15,45,true},{5,45,true}};
  host.children={hole};std::vector<Polygon> parts={rectangle(0,0,12,12,1),host};
  auto result=placePartsBitmap({sheet},parts,cfg);
  require(result.unplaced.size()==1,"Hole bounding box is not a containment test");
  valid(sheet,parts,result,cfg);
}
void rowsAndGpuPolicy() {
  auto cfg=settings();cfg.spacing=5;cfg.holeSpacing=5;cfg.gpuEnabled=true;
  cfg.gpuFallbackToCpu=true;cfg.gpuDevice=1000;
  auto sheet=rectangle(0,0,100,100,99);sheet.children={rectangle(42,42,12,12)};
  std::vector<Polygon> parts;
  for(int i=0;i<6;++i) parts.push_back(rectangle(0,0,10,10,i+1));
  BitmapNestingStats stats;auto result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty() && stats.patternPlacements==6,"Repeated pattern should fit all six");
  require(!stats.gpuFallbackReason.empty(),"Fast patterns must attempt the enabled GPU");
  require(stats.gpuBatches==0,"An invalid GPU must not report successful GPU work");
  valid(sheet,parts,result,cfg);
  cfg.gpuFallbackToCpu=false;bool threw=false;
  try {placePartsBitmap({sheet},parts,cfg);} catch(const std::runtime_error&) {threw=true;}
  require(threw,"Fast patterns must honor strict GPU failure policy");
  cfg.spacing=1000000;cfg.holeSpacing=1000000;cfg.gpuEnabled=false;
  cfg.bitmapResolutionMm=0.000001;auto tinySheet=rectangle(0,0,0.0001,0.0001,99);
  parts.clear();for(int i=0;i<6;++i) parts.push_back(rectangle(0,0,0.00001,0.00001,i+1));
  cfg.sheetSpacing=0;result=placePartsBitmap({tinySheet},parts,cfg);
  require(result.unplaced.size()>=5,"Huge spacing must not overflow pattern stride");
}
void interrupted() {
  auto cfg=settings();cfg.threads=4;
  std::atomic<int> checks{0};
  cfg.stopRequested=[&]{return checks.fetch_add(1,std::memory_order_relaxed)>700;};
  std::vector<Polygon> parts;for(int i=0;i<1000;++i) parts.push_back(rectangle(0,0,10,10,i+1));
  BitmapNestingStats stats;auto result=placePartsBitmap({rectangle(0,0,450,450,9999)},parts,cfg,&stats);
  size_t placed=0;for(const auto& s:result.placements) placed+=s.sheetplacements.size();
  require(stats.cancelled && placed+result.unplaced.size()==parts.size(),"Cancellation lost copies");
  std::vector<int> ids;
  for(const auto& s:result.placements) for(const auto& p:s.sheetplacements) ids.push_back(*p.id);
  for(const auto& p:result.unplaced) ids.push_back(*p.id);
  std::sort(ids.begin(),ids.end());
  for(size_t i=0;i<ids.size();++i) require(ids[i]==int(i+1),"Cancellation duplicated or omitted a copy");
}
void mixedOneAngleWorkers() {
  auto cfg=settings();auto sheet=rectangle(0,0,450,450,9999);
  std::vector<Polygon> parts;
  for(int i=0;i<1000;++i) parts.push_back(rectangle(0,0,10,10,i+1));
  parts.push_back(rectangle(0,0,9,7,1001));
  BitmapNestingStats serialStats;
  const auto serial=placePartsBitmap({sheet},parts,cfg,&serialStats);
  require(serial.unplaced.empty(),"The 1000-copy plus singleton reference must fit");
  require(serialStats.cpuWorkersUsed==1,"Single-thread mode must use one CPU slot");
  valid(sheet,parts,serial,cfg);
  for(int threads:{4,12}) {
    cfg.threads=threads;
    // Changing the singleton's input position must not suppress acceleration.
    if(threads==12) std::rotate(parts.begin(),parts.end()-1,parts.end());
    BitmapNestingStats stats;const auto result=placePartsBitmap({sheet},parts,cfg,&stats);
    sameLayout(serial,result);
    require(stats.proposalWorkersPerTrial>1,"One-angle jobs must allocate CPU helpers");
    require(stats.candidateWorkersUsed>1,"One-angle jobs must perform validation on multiple CPU workers");
    require(stats.cpuWorkersUsed>1 && stats.cpuWorkersUsed<=size_t(threads),"CPU pool must respect the requested thread budget");
    require(stats.candidateWorkersUsed<=stats.cpuWorkersUsed,"Validation workers must belong to the CPU pool");
  }
}
void singletonWindowExpansion() {
  auto cfg=settings();cfg.threads=4;
  BitmapNestingStats stats;
  const auto result=placePartsBitmap({rectangle(0,0,40,40,99)},{rectangle(0,0,10,8,1)},cfg,&stats);
  require(result.unplaced.empty() && result.placements.size()==1 && result.placements.front().sheetplacements.size()==1,
          "Singleton window expansion must place its part");
  const auto& placed=result.placements.front().sheetplacements.front();
  require(stats.searchWindowExpansions>0 && placed.x==2 && placed.y==2,
          "Temporary search-window misses must not become permanent bitmap rejections");
}
void repeatedInsertsAfterHoleFills() {
  auto cfg=settings();auto sheet=rectangle(0,0,350,350,9999);
  auto host=rectangle(0,0,88,88,1);host.children={rectangle(13,13,62,62)};
  std::vector<Polygon> parts{host};
  for(int i=0;i<64;++i) {
    auto p=rectangle(0,0,16,27,i+2);
    p.allowedAngles.clear();for(int angle=0;angle<360;++angle) p.allowedAngles.push_back(angle);
    parts.push_back(std::move(p));
  }
  BitmapNestingStats serialStats;
  const auto serial=placePartsBitmap({sheet},parts,cfg,&serialStats);
  require(serial.unplaced.empty() && serialStats.holePlacements>0 && serialStats.patternPlacements>0,
          "Repeated inserts must fill the cavity then continue outside");
  valid(sheet,parts,serial,cfg);
  cfg.threads=12;BitmapNestingStats parallelStats;
  const auto result=placePartsBitmap({sheet},parts,cfg,&parallelStats);
  sameLayout(serial,result);
  require(parallelStats.candidateWorkersUsed>1,"Many-angle cavity validation must use CPU helpers");
}
void gpuFastPathsWhenAvailable() {
  std::vector<GpuDeviceInfo> devices;
  try {devices=listGpuDevices();} catch(const std::exception& e) {
    std::cout<<"Skipping GPU hardware checks: "<<e.what()<<"\n";return;
  }
  if(devices.empty()) {std::cout<<"Skipping GPU hardware checks: no OpenCL GPU\n";return;}
  // Exercise shifted words and stock edges independently of candidate ordering.
  {
    GpuBitmap gpu(devices.front().index);
    constexpr int width=130,height=6,stride=3,maskWidth=65,maskHeight=3,maskStride=2;
    std::vector<uint64_t> material(stride*height),occupied(stride*height),bits(maskStride*maskHeight);
    for(int y=0;y<height;++y) {
      material[y*stride]=~uint64_t(0);material[y*stride+1]=~uint64_t(0);material[y*stride+2]=3;
    }
    material[4*stride+1]&=~(uint64_t(1)<<63);
    occupied[1*stride+1]=1;occupied[4*stride]=2;
    bits[0]=(uint64_t(1)<<63)|1;bits[1]=1;
    bits[2]=(uint64_t(1)<<62)|2;bits[4]=uint64_t(1)<<32;
    const std::vector<GpuMaskInfo> masks={{maskWidth,maskHeight,maskStride,0}};
    gpu.setSheet(width,height,material);gpu.setOccupancy(occupied);gpu.setMasks(masks,bits);
    std::vector<GpuCandidate> candidates;
    for(int x:{-1,0,1,63,64,65,66}) for(int y:{0,1,3,4}) candidates.push_back({x,y,0});
    candidates.push_back({0,0,1});
    const auto flags=gpu.filter(candidates);
    for(size_t i=0;i<candidates.size();++i) {
      const auto& c=candidates[i];
      bool expected=c.rotation==0 && c.x>=0 && c.y>=0 && c.x+maskWidth<=width && c.y+maskHeight<=height;
      if(expected) for(int y=0;y<maskHeight;++y) for(int x=0;x<maskWidth;++x) {
        if(!(bits[y*maskStride+x/64]&(uint64_t(1)<<(x%64)))) continue;
        const int sx=c.x+x,sy=c.y+y;const uint64_t bit=uint64_t(1)<<(sx%64);
        if(!(material[sy*stride+sx/64]&bit) || (occupied[sy*stride+sx/64]&bit)) expected=false;
      }
      require(bool(flags[i])==expected,"GPU shifted-word bitmap filter differs from pixel reference");
    }
  }
  auto check=[&](const Polygon& sheet,const std::vector<Polygon>& parts,size_t expectedHoles,bool expectParallel) {
    auto cfg=settings();cfg.threads=4;
    const auto cpu=placePartsBitmap({sheet},parts,cfg);
    require(cpu.unplaced.empty(),"GPU reference job must fit");
    cfg.gpuEnabled=true;cfg.gpuFallbackToCpu=false;cfg.gpuDevice=devices.front().index;
    BitmapNestingStats stats;const auto gpu=placePartsBitmap({sheet},parts,cfg,&stats);
    require(stats.gpuBatches>0 && stats.gpuCandidates>0 && !stats.gpuDevice.empty(),
            "Enabled GPU must process fast-path candidates");
    require(stats.gpuFallbackReason.empty(),"Hardware checks must run on the GPU without fallback");
    require(stats.holePlacements==expectedHoles,"GPU changed hole placement");
    if(expectParallel) require(stats.candidateWorkersUsed>1,"GPU filtering must retain parallel CPU validation");
    sameLayout(cpu,gpu);valid(sheet,parts,gpu,cfg);
  };
  std::vector<Polygon> repeated;
  for(int i=0;i<32;++i) repeated.push_back(rectangle(0,0,10,10,i+1));
  check(rectangle(0,0,100,100,99),repeated,0,true);
  auto host=rectangle(0,0,40,40,2);host.children={rectangle(10,10,20,20)};
  check(rectangle(0,0,44,44,99),{rectangle(0,0,10,10,1),host},1,false);
  check(rectangle(0,0,40,40,99),{rectangle(0,0,10,8,1)},0,false);
}
void timedAlternatives() {
  auto cfg=settings();cfg.mode=SearchMode::Timed;cfg.bitmapTrials=4;cfg.threads=4;
  cfg.timeLimitSeconds=2;
  auto sheet=rectangle(0,0,200,200,99);std::vector<Polygon> parts;
  for(int i=0;i<4;++i) parts.push_back(rectangle(0,0,10,10,i+1));
  for(int i=0;i<4;++i) {
    auto host=rectangle(0,0,40,40,i+10);host.children={rectangle(10,10,20,20)};parts.push_back(host);
  }
  size_t holeTrialInserts=0;BitmapNestingStats stats;
  auto result=placePartsBitmap({sheet},parts,cfg,&stats,{},[&](const auto& layout,const auto& trial) {
    if(trial.selectedTrial==1) holeTrialInserts=trial.holePlacements;
    valid(sheet,parts,layout,cfg);
  });
  require(stats.completedTrials==4 && holeTrialInserts==4,"Timed search must compare the hole strategy with alternatives");
  require(result.unplaced.empty(),"Timed alternatives lost copies");
  cfg.bitmapTrials=2;
  sheet=rectangle(0,0,44,44,99);
  parts={rectangle(0,0,10,10,1),rectangle(0,0,40,40,2)};
  parts[1].children={rectangle(10,10,20,20)};
  result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty() && stats.completedTrials==2 && stats.holePlacements==1,
          "Small timed jobs with holes still need the holes-first alternative");
  valid(sheet,parts,result,cfg);
}
void gpuFailurePolicy() {
  auto cfg=settings();cfg.gpuEnabled=true;cfg.gpuDevice=1000;
  auto sheet=rectangle(0,0,5,5,99);auto part=rectangle(0,0,10,10);
  BitmapNestingStats stats;auto result=placePartsBitmap({sheet},{part},cfg,&stats);
  require(result.unplaced.size()==1 && !stats.gpuFallbackReason.empty(),"GPU grid search must preserve CPU fallback");
  cfg.gpuFallbackToCpu=false;bool threw=false;
  try {placePartsBitmap({sheet},{part},cfg);} catch(const std::runtime_error&) {threw=true;}
  require(threw,"GPU grid search must preserve strict failure policy");
}
void freeRectangleProof() {
  std::mt19937 random(729);
  for(int trial=0;trial<30;++trial) {
    detail::FreeRectangles space(12,9);
    bool occupied[9][12]{};
    for(int step=0;step<12;++step) {
      detail::PixelRect r{int(random()%14)-1,int(random()%11)-1,1+int(random()%5),1+int(random()%5)};
      space.occupy(r);
      for(int y=0;y<9;++y) for(int x=0;x<12;++x)
        if(x>=r.x && y>=r.y && x<r.x+r.width && y<r.y+r.height) occupied[y][x]=true;
      for(int h=1;h<=9;++h) for(int w=1;w<=12;++w) {
        bool possible=false;
        for(int y=0;y+h<=9;++y) for(int x=0;x+w<=12;++x) {
          bool clear=true;
          for(int dy=0;dy<h;++dy) for(int dx=0;dx<w;++dx) clear=clear && !occupied[y+dy][x+dx];
          possible=possible || clear;
        }
        require(space.fits(w,h)==possible,"Free-rectangle proof differs from exhaustive bitmap reference");
      }
    }
  }
}
void mixedPanelsAndMultipleSheets() {
  auto cfg=settings();cfg.spacing=6.5;cfg.holeSpacing=6.5;cfg.sheetSpacing=0;cfg.threads=4;
  std::vector<Polygon> sheets,parts;
  for(int i=0;i<4;++i) sheets.push_back(rectangle(0,0,2000,2800,100+i));
  for(int i=0;i<4;++i) {
    auto p=rectangle(0,0,1250+i*10,1800,i+1);p.allowedAngles={0,90,180,270};parts.push_back(p);
  }
  for(int i=0;i<12;++i) {auto p=rectangle(0,0,200+i,250,i+10);p.allowedAngles={0,90,180,270};parts.push_back(p);}
  BitmapNestingStats stats;
  const auto cpu=placePartsBitmap(sheets,parts,cfg,&stats);
  require(cpu.unplaced.empty(),"Mixed panels must advance to remaining sheets");
  require(stats.fineFallbacks==0 && stats.candidatesExamined<200000,"Impossible panels entered the exhaustive pixel scan");
  for(const auto& placement:cpu.placements) {
    PlacementResult one;one.placements={placement};
    valid(sheets.front(),parts,one,cfg);
  }
  std::vector<GpuDeviceInfo> devices;
  try {devices=listGpuDevices();} catch(const std::exception&) {return;}
  if(devices.empty()) return;
  cfg.gpuEnabled=true;cfg.gpuFallbackToCpu=false;cfg.gpuDevice=devices.front().index;
  const auto gpu=placePartsBitmap(sheets,parts,cfg,&stats);
  require(stats.gpuBatches>0 && stats.gpuFallbackReason.empty(),"Multi-sheet GPU job did not use the GPU");
  sameLayout(cpu,gpu);
}
void bottomLeftSearch() {
  auto cfg=settings();cfg.bitmapBottomLeft=true;cfg.spacing=6.5;cfg.holeSpacing=6.5;cfg.sheetSpacing=0;cfg.threads=1;
  auto sheet=rectangle(0,0,100,80,99);
  std::vector<Polygon> parts{rectangle(0,0,10,10,1),rectangle(0,0,20,20,2),rectangle(0,0,40,30,3)};
  for(auto& p:parts)p.allowedAngles={0};
  BitmapNestingStats stats;const auto result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty() && result.placements.size()==1,"Bottom-left lost furniture parts");
  const auto& placed=result.placements.front().sheetplacements;
  require(placed[0].id==3 && placed[0].x==0 && placed[0].y==0,"Largest part must start bottom-left");
  require(placed[1].id==2 && std::abs(placed[1].x-46.5)<0.001 && placed[1].y==0,"Next part must use the lowest row with subpixel clearance");
  require(stats.fineFallbacks==0 && stats.candidatesExamined<500,"Bottom-left must avoid grid scans");
  valid(sheet,parts,result,cfg);
  cfg.threads=12;sameLayout(result,placePartsBitmap({sheet},parts,cfg,&stats));
  auto oversized=rectangle(0,0,101,81,4);oversized.allowedAngles={0};
  auto failure=placePartsBitmap({sheet},{oversized},cfg,&stats);
  require(failure.unplaced.size()==1 && stats.fineFallbacks==0,"Failed contact search must return without exhaustive scans");
}
void fractionalHoleAndSheetMargins() {
  auto cfg=settings();cfg.bitmapBottomLeft=true;cfg.spacing=6.5;cfg.holeSpacing=6.5;cfg.sheetSpacing=2.25;
  auto sheet=rectangle(100,-50,80,80,99);
  auto host=rectangle(0,0,60,60,1);host.allowedAngles={90};host.children={rectangle(20,20,20,20)};
  std::vector<Polygon> parts{rectangle(0,0,4,4,2),host};
  BitmapNestingStats stats;auto result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty() && stats.vectorRefinementCompleted,"Fractional hole refinement must finish");
  valid(sheet,parts,result,cfg);auto shapes=transformed(parts,result);
  require(std::abs(distance(shapes[0],sheet)-2.25)<0.001,"Fractional sheet margin lost");
  require(std::abs(distance(shapes[0].children[0],shapes[1])-6.5)<0.001,"Fractional hole margin lost");
  require(result.placements.front().sheetplacements.front().rotation==90,"Refinement must preserve allowed orientation");
}
void modeSearchAndRefinement() {
  struct Sink:EventSink {
    void onTestStart(const std::vector<Polygon>&,const std::vector<Polygon>&,const Config&,int)override{}
    void onProgress(int,double)override{}
    void onResult(const PlacementResult&)override{}
  }sink;
  auto req=parseNestingJson(R"({"config":{"mode":"first","spacing":6.5,"threads":4,"gpu":false},"sheets":[{"width":100,"height":80}],"parts":[{"points":[[0,0],[10,0],[10,10],[0,10]]},{"points":[[0,0],[40,0],[40,30],[0,30]]}]})");
  auto first=BackgroundOrchestrator().runWithStats(req,sink);
  require(first.bitmapStats.fineFallbacks==0 && first.bitmapStats.trials.front().strategy=="first_bottom_left","First must select bottom-left automatically");
  auto shapes=transformed(req.individual.placement,first.placement);
  require(first.placement.unplaced.empty() && std::abs(distance(shapes[0],shapes[1])-6.5)<0.001,"Vector refinement must achieve fractional clearance");
  valid(req.sheets.front(),req.individual.placement,first.placement,req.config);
  req.config.mode=SearchMode::Timed;req.config.timeLimitSeconds=0.3;req.config.continuousRoundSeconds=0.05;
  auto timed=runTimedNesting(req,[]{return false;});
  require(timed.bitmapStats.searchIterations>1,"Timed mode must explore multiple restarts");
  require(!improvesLayout(layoutQuality(req.sheets,first.placement,first.bitmapStats),layoutQuality(req.sheets,timed.placement,timed.bitmapStats)),"Timed result regressed from initial contact layout");
  valid(req.sheets.front(),req.individual.placement,timed.placement,req.config);
}
int main() {
  try {
    fractionalHoleAndSheetMargins();modeSearchAndRefinement();bottomLeftSearch();scanlines();freeRectangleProof();mixedPanelsAndMultipleSheets();duplicateContours();holesFirst();rotatedAndMultipleHoles();concavePocket();rowsAndGpuPolicy();interrupted();mixedOneAngleWorkers();repeatedInsertsAfterHoleFills();singletonWindowExpansion();timedAlternatives();gpuFailurePolicy();gpuFastPathsWhenAvailable();
    std::cout<<"All nesting regression checks passed\n";return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
