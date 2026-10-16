#pragma once
#include "clinesting/geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace clinesting {
// Direction affects search/ranking only. World geometry and angle rules stay fixed.
struct FillOrder {
  Bounds bounds;
  int axis{1};
  bool reverse{false},enabled{true};
  double depth(const Bounds& part) const {
    if(axis==0) return reverse ? bounds.x+bounds.width-part.x : part.x+part.width-bounds.x;
    return reverse ? bounds.y+bounds.height-part.y : part.y+part.height-bounds.y;
  }
  double length() const {return axis==0 ? bounds.width : bounds.height;}
  double cost(double depthMm,double stockArea) const {
    return enabled && length()>0 ? std::clamp(depthMm/length(),0.0,1.0)*stockArea : 0;
  }
  // Permute a finite grid without adding/removing any origin. Both GPU and CPU
  // consume these same physical coordinates, including the same step lattice.
  std::pair<int,int> origin(uint64_t cell,uint64_t columns,uint64_t rows,int step,int edge=0) const {
    uint64_t x,y;
    if(!enabled || axis==0) {x=cell/rows;y=cell%rows;}
    else {x=cell%columns;y=cell/columns;}
    if(enabled && reverse) {if(axis==0)x=columns-1-x;else y=rows-1-y;}
    return {int(x)*step+edge,int(y)*step+edge};
  }
};

namespace detail {
// Half-plane clipping retains signed area even if a concave ring splits into
// multiple lobes joined by cancelling segments on the clipping line.
inline double bandRingArea(const Polygon& polygon,int axis,double boundary,bool lower) {
  std::vector<Point> output;
  const auto& ring=polygon.points;
  if(ring.empty()) return 0;
  auto value=[&](const Point& p){return axis==0?p.x:p.y;};
  auto inside=[&](const Point& p){return lower?value(p)<=boundary:value(p)>=boundary;};
  auto previous=ring.back();bool wasInside=inside(previous);
  for(const auto& current:ring) {
    const bool isInside=inside(current);
    if(isInside!=wasInside) {
      const double t=(boundary-value(previous))/(value(current)-value(previous));
      output.push_back({previous.x+t*(current.x-previous.x),previous.y+t*(current.y-previous.y),true});
    }
    if(isInside)output.push_back(current);
    previous=current;wasInside=isInside;
  }
  double twice=0;
  for(size_t i=0;i<output.size();++i) {
    const auto& a=output[i];const auto& b=output[(i+1)%output.size()];
    twice+=a.x*b.y-a.y*b.x;
  }
  double area=std::abs(twice)/2;
  for(const auto& hole:polygon.children)area-=bandRingArea(hole,axis,boundary,lower);
  return std::max(0.0,area);
}
}

inline FillOrder sheetFillOrder(const Polygon& sheet,FillDirection direction) {
  FillOrder result;result.bounds=getPolygonBounds(sheet.points);
  result.enabled=direction!=FillDirection::Automatic;
  if(!result.enabled)return result;
  struct Side {int axis;bool reverse;double width;};
  const auto& b=result.bounds;
  // Average usable cross-section in the outer quarter, rather than one tip
  // vertex. Holes are excluded; tie order is bottom, top, left, right.
  std::array<Side,4> sides{{{1,false,0},{1,true,0},{0,false,0},{0,true,0}}};
  for(auto& side:sides) {
    const double start=side.axis==0?b.x:b.y, length=side.axis==0?b.width:b.height;
    const double band=length/4;
    side.width=band>0 ? detail::bandRingArea(sheet,side.axis,start+(side.reverse?length-band:band),!side.reverse)/band : 0;
  }
  const bool wide=direction==FillDirection::WideToNarrow;
  const Side* selected=&sides.front();
  for(const auto& side:sides) {
    const double tolerance=std::max({1.0,std::abs(selected->width),std::abs(side.width)})*1e-9;
    if(wide?side.width>selected->width+tolerance:side.width<selected->width-tolerance)selected=&side;
  }
  result.axis=selected->axis;result.reverse=selected->reverse;
  return result;
}

inline double fillDirectionCost(const Polygon& sheet,const std::vector<Polygon>& geometry,FillDirection direction) {
  const auto order=sheetFillOrder(sheet,direction);
  double depth=0;
  for(const auto& part:geometry)depth=std::max(depth,order.depth(getPolygonBounds(part.points)));
  return order.cost(depth,polygonMaterialArea(sheet));
}

// Seed contact-only search inside tapered stock, where bounding-box corners
// can all be outside the material. These are proposals, never fit proofs.
template<class Propose>
void directionSeeds(const Polygon& sheet,const FillOrder& flow,int width,int height,
                    double resolution,double margin,Propose propose) {
  if(!flow.enabled)return;
  const auto& b=flow.bounds;
  const double primary=flow.axis==0?width*resolution:height*resolution;
  const double cross=flow.axis==0?height*resolution:width*resolution;
  const double start=flow.axis==0?b.x:b.y;
  const double crossStart=flow.axis==0?b.y:b.x;
  const double span=flow.length()-primary-2*margin;
  if(span<0)return;
  auto section=[&](double position) {
    double lo=INFINITY,hi=-INFINITY;
    const auto& ring=sheet.points;
    for(size_t i=0;i<ring.size();++i) {
      const auto& a=ring[i];const auto& z=ring[(i+1)%ring.size()];
      const double p=flow.axis==0?a.x:a.y,q=flow.axis==0?z.x:z.y;
      const double u=flow.axis==0?a.y:a.x,v=flow.axis==0?z.y:z.x;
      if(position<std::min(p,q)-1e-8 || position>std::max(p,q)+1e-8)continue;
      if(std::abs(p-q)<1e-9) {lo=std::min({lo,u,v});hi=std::max({hi,u,v});}
      else {const double at=u+(v-u)*(position-p)/(q-p);lo=std::min(lo,at);hi=std::max(hi,at);}
    }
    return std::pair{lo,hi};
  };
  for(int i=0;i<=48;++i) {
    const double at=std::round((margin+span*i/48)/resolution)*resolution+start;
    double lo=-INFINITY,hi=INFINITY;
    for(double p:{at,at+primary/2,at+primary}) {
      const auto [a,z]=section(p);lo=std::max(lo,a+margin);hi=std::min(hi,z-margin-cross);
    }
    if(!std::isfinite(lo)||!std::isfinite(hi)||lo>hi)continue;
    for(double c:{std::ceil((lo-crossStart)/resolution),std::floor((hi-crossStart)/resolution),std::round(((lo+hi)/2-crossStart)/resolution)}) {
      const int primaryPixel=int(std::llround((at-start)/resolution)),crossPixel=int(c);
      propose(flow.axis==0?primaryPixel:crossPixel,flow.axis==0?crossPixel:primaryPixel);
    }
  }
}
}
