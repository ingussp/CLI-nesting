#include "clinesting/reusable_offcut.hpp"
#include "clinesting/geometry.hpp"
#include <clipper2/clipper.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <map>
#include <limits>
#include <unordered_map>
namespace clinesting {
namespace {
using namespace Clipper2Lib;
constexpr double scale=1000000.0;
void materialPaths(const Polygon& p,Paths64& paths,bool hole=false) {
  auto path=toClipperCoordinates(p.points,scale);
  if(IsPositive(path)==hole) std::reverse(path.begin(),path.end());
  paths.push_back(std::move(path));
  for(const auto& child:p.children) materialPaths(child,paths,!hole);
}
// Offset outer and hole boundaries independently, retaining nested islands.
Paths64 bufferedMaterial(const Polygon& p,double outerOffset,double holeInset,double tolerance) {
  auto path=toClipperCoordinates(p.points,scale);
  if(!IsPositive(path)) std::reverse(path.begin(),path.end());
  auto offset=[&](const Path64& outline,double delta) {
    return delta==0 ? Paths64{outline} : InflatePaths(Paths64{outline},delta*scale,
        JoinType::Round,EndType::Polygon,2.0,tolerance);
  };
  auto material=offset(path,outerOffset);
  for(const auto& hole:p.children) {
    auto outline=toClipperCoordinates(hole.points,scale);
    if(!IsPositive(outline)) std::reverse(outline.begin(),outline.end());
    auto opening=offset(outline,-holeInset);
    for(const auto& island:hole.children)
      opening=Difference(opening,bufferedMaterial(island,outerOffset,holeInset,tolerance),FillRule::NonZero);
    material=Difference(material,opening,FillRule::NonZero);
  }
  return material;
}
struct Component {Paths64 paths;double area;};
// Clipper can return weakly simple rings whose lobes touch at one vertex.
// Split at repeated integer vertices without moving or simplifying any edge.
Paths64 splitTouchingRing(const Path64& path) {
  Paths64 rings;if(path.empty()) return rings;
  Path64 active;std::map<std::pair<int64_t,int64_t>,size_t> positions;
  for(size_t i=0;i<=path.size();++i) {
    const auto& p=path[i%path.size()];const auto key=std::make_pair(p.x,p.y);
    const auto found=positions.find(key);
    if(found==positions.end()) {positions.emplace(key,active.size());active.push_back(p);continue;}
    const size_t at=found->second;
    Path64 ring(active.begin()+at,active.end());
    if(ring.size()>=3 && Area(ring)!=0) rings.push_back(std::move(ring));
    for(size_t j=at+1;j<active.size();++j) positions.erase({active[j].x,active[j].y});
    active.resize(at+1);
  }
  return rings;
}
std::vector<Component> splitTouchingComponents(const std::vector<Component>& input) {
  std::vector<Component> output;
  for(const auto& component:input) {
    std::vector<Component> outers;Paths64 holes;
    for(const auto& path:component.paths) for(auto& ring:splitTouchingRing(path)) {
      const double area=Area(ring);
      if(area>0) outers.push_back({{std::move(ring)},area});
      else holes.push_back(std::move(ring));
    }
    for(auto& hole:holes) {
      size_t owner=outers.size();double ownerArea=std::numeric_limits<double>::max();
      for(size_t i=0;i<outers.size();++i) {
        bool inside=false;
        for(const auto& p:hole) {
          const auto location=PointInPolygon(p,outers[i].paths.front());
          if(location==PointInPolygonResult::IsOn) continue;
          inside=location==PointInPolygonResult::IsInside;break;
        }
        const double area=Area(outers[i].paths.front());
        if(inside && area<ownerArea) {owner=i;ownerArea=area;}
      }
      if(owner==outers.size()) throw std::runtime_error("Offcut hole has no enclosing component");
      outers[owner].area-=std::abs(Area(hole));outers[owner].paths.push_back(std::move(hole));
    }
    for(auto& outer:outers) output.push_back(std::move(outer));
  }
  return output;
}
void collect(const PolyPath64& node,std::vector<Component>& components) {
  for(const auto& child:node) {
    if(!child->IsHole()) {
      Component c{{child->Polygon()},std::abs(Area(child->Polygon()))};
      for(const auto& hole:*child) {
        c.paths.push_back(hole->Polygon());c.area-=std::abs(Area(hole->Polygon()));
      }
      components.push_back(std::move(c));
    }
    collect(*child,components);
  }
}
std::vector<Component> componentsOf(const Paths64& paths) {
  PolyTree64 tree;BooleanOp(ClipType::Union,FillRule::NonZero,paths,{},tree);
  std::vector<Component> components;collect(tree,components);return splitTouchingComponents(components);
}
}
ReusableOffcut measureReusableOffcut(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const PlacementResult& layout,
    double minWidthMm,const std::function<bool()>& stop) {
  Config config;config.reusableOffcutMinWidthMm=minWidthMm;
  return measureReusableOffcut(sheets,parts,layout,config,stop);
}
ReusableOffcut measureReusableOffcut(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const PlacementResult& layout,
    const Config& config,const std::function<bool()>& stop) {
  const double minWidthMm=config.reusableOffcutMinWidthMm;
  if(!std::isfinite(minWidthMm) || minWidthMm<=0 || minWidthMm>1000000)
    throw std::invalid_argument("Reusable offcut minimum width must be in (0,1000000]");
  for(double gap:{config.spacing,config.sheetSpacing,config.holeSpacing})
    if(!std::isfinite(gap) || gap<0)
      throw std::invalid_argument("Offcut clearances must be finite and nonnegative");
  const auto start=std::chrono::steady_clock::now();
  ReusableOffcut result;result.minWidthMm=minWidthMm;
  result.spacingMm=config.spacing;result.sheetSpacingMm=config.sheetSpacing;result.holeSpacingMm=config.holeSpacing;
  auto finish=[&](bool evaluated) {result.evaluated=evaluated;
    result.elapsedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return result;};
  auto stopped=[&] {return stop && stop();};
  std::unordered_map<int,const Polygon*> originals;
  for(const auto& part:parts) if(part.id) originals.emplace(*part.id,&part);
  const double radius=minWidthMm*scale/2;
  // Publication-only evaluation uses a 0.001 mm arc tolerance.
  result.arcToleranceMm=std::min(0.001,minWidthMm/100);
  const double tolerance=result.arcToleranceMm*scale;
  for(const auto& placedSheet:layout.placements) {
    if(stopped()) return finish(false);
    if(placedSheet.sheetplacements.empty()) continue;
    const auto sheet=std::find_if(sheets.begin(),sheets.end(),[&](const auto& p){return p.id==placedSheet.sheetid;});
    if(sheet==sheets.end()) throw std::invalid_argument("Unknown sheet in offcut evaluation");
    Paths64 stock,occupied,reserved;materialPaths(*sheet,stock);
    for(const auto& placed:placedSheet.sheetplacements) {
      if(stopped()) return finish(false);
      const auto& source=*originals.at(placed.id.value());
      const auto transformed=shiftPolygon(rotatePolygon(source,placed.rotation),{placed.x,placed.y,true});
      materialPaths(transformed,occupied);
      if(config.spacing>0 || config.holeSpacing>0) {
        auto clearance=bufferedMaterial(transformed,config.spacing,config.holeSpacing,tolerance);
        reserved.insert(reserved.end(),clearance.begin(),clearance.end());
      }
    }
    const auto free=Difference(stock,occupied,FillRule::NonZero);
    result.freeArea+=std::abs(Area(free))/(scale*scale);
    if(stopped()) return finish(false);
    auto available=free;
    if(config.spacing>0 || config.sheetSpacing>0 || config.holeSpacing>0) {
      const auto allowedStock=bufferedMaterial(*sheet,-config.sheetSpacing,-config.holeSpacing,tolerance);
      available=Intersect(free,Difference(allowedStock,
          config.spacing>0 || config.holeSpacing>0 ? reserved : occupied,FillRule::NonZero),FillRule::NonZero);
    }
    result.availableArea+=std::abs(Area(available))/(scale*scale);
    result.clearanceExcludedArea=std::max(0.0,result.freeArea-result.availableArea);
    const auto eroded=InflatePaths(available,-radius,JoinType::Round,EndType::Polygon,2.0,tolerance);
    auto cores=componentsOf(eroded);result.components+=cores.size();
    std::stable_sort(cores.begin(),cores.end(),[](const auto& a,const auto& b){return a.area>b.area;});
    for(const auto& core:cores) {
      if(stopped()) return finish(false);
      const auto bounds=GetBounds(core.paths);
      const double upper=(double(bounds.Width())+2*radius)*(double(bounds.Height())+2*radius)/(scale*scale);
      if(upper<=result.area) continue;
      // Recover separately: reconnecting all dilated cores would recreate the
      // narrow bridges that the width test was meant to exclude.
      const auto grown=InflatePaths(core.paths,radius,JoinType::Round,EndType::Polygon,2.0,tolerance);
      const auto recovered=Intersect(grown,available,FillRule::NonZero);
      for(const auto& piece:componentsOf(recovered)) {
        const double area=piece.area/(scale*scale);
        if(area<=result.area) continue;
        // A dilation may reach a different nearby island across occupied material.
        // Count only the connected recovered piece containing this core.
        if(std::abs(Area(Intersect(piece.paths,core.paths,FillRule::NonZero)))<1) continue;
        result.area=area;result.coreArea=core.area/(scale*scale);result.sheetId=sheet->id;
        result.contour={};result.contour.points=toNestCoordinates(piece.paths.front(),scale);
        for(size_t i=1;i<piece.paths.size();++i) {
          Polygon hole;hole.points=toNestCoordinates(piece.paths[i],scale);result.contour.children.push_back(std::move(hole));
        }
      }
    }
  }
  return finish(true);
}
}
