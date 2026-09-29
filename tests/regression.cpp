#include "clinesting/bitmap_nesting.hpp"
#include "clinesting/geometry.hpp"
#include "clinesting/json_io.hpp"
#include "raster_scanline.hpp"
#include <algorithm>
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
void rowsAndLazyGpu() {
  auto cfg=settings();cfg.spacing=5;cfg.holeSpacing=5;cfg.gpuEnabled=true;
  cfg.gpuFallbackToCpu=false;cfg.gpuDevice=1000; // Must never initialize for a successful pattern.
  auto sheet=rectangle(0,0,100,100,99);sheet.children={rectangle(42,42,12,12)};
  std::vector<Polygon> parts;
  for(int i=0;i<6;++i) parts.push_back(rectangle(0,0,10,10,i+1));
  BitmapNestingStats stats;auto result=placePartsBitmap({sheet},parts,cfg,&stats);
  require(result.unplaced.empty() && stats.patternPlacements==6,"Repeated pattern should fit all six");
  require(stats.gpuBatches==0 && stats.gpuDevice.empty(),"Unnecessary GPU initialization");
  valid(sheet,parts,result,cfg);
  cfg.spacing=1000000;cfg.holeSpacing=1000000;cfg.gpuEnabled=false;
  cfg.bitmapResolutionMm=0.000001;auto tinySheet=rectangle(0,0,0.0001,0.0001,99);
  parts.clear();for(int i=0;i<6;++i) parts.push_back(rectangle(0,0,0.00001,0.00001,i+1));
  cfg.sheetSpacing=0;result=placePartsBitmap({tinySheet},parts,cfg);
  require(result.unplaced.size()>=5,"Huge spacing must not overflow pattern stride");
}
void interrupted() {
  auto cfg=settings();int checks=0;cfg.stopRequested=[&]{return ++checks>20;};
  std::vector<Polygon> parts;for(int i=0;i<100;++i) parts.push_back(rectangle(0,0,10,10,i+1));
  BitmapNestingStats stats;auto result=placePartsBitmap({rectangle(0,0,100,100,99)},parts,cfg,&stats);
  size_t placed=0;for(const auto& s:result.placements) placed+=s.sheetplacements.size();
  require(stats.cancelled && placed+result.unplaced.size()==parts.size(),"Cancellation lost copies");
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
  require(result.unplaced.size()==1 && !stats.gpuFallbackReason.empty(),"Lazy GPU must preserve CPU fallback");
  cfg.gpuFallbackToCpu=false;bool threw=false;
  try {placePartsBitmap({sheet},{part},cfg);} catch(const std::runtime_error&) {threw=true;}
  require(threw,"Lazy GPU must preserve strict failure policy");
}
int main() {
  try {
    scanlines();duplicateContours();holesFirst();rotatedAndMultipleHoles();concavePocket();rowsAndLazyGpu();interrupted();timedAlternatives();gpuFailurePolicy();
    std::cout<<"All nesting regression checks passed\n";return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
