#pragma once
#include "clinesting/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
#include <optional>

namespace clinesting {
// Same exact predicate as the unindexed reference implementation.
double segmentDistanceSquared(const Point&,const Point&,const Point&,const Point&);
namespace detail {
// Immutable edge BVH for one contour. Query translations do not rebuild the tree.
class ContourIndex {
  struct Box { double x0,y0,x1,y1; };
  struct Edge { Point a,b; Box box; };
  struct Node { Box box; size_t begin,end,left{0},right{0}; };
  std::vector<Edge> edges_;
  std::vector<Node> nodes_;
  static Box box(const Point& a,const Point& b) {
    return {std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)};
  }
  static double distanceSquared(const Box& a,const Box& b) {
    const double x=std::max({a.x0-b.x1,b.x0-a.x1,0.0});
    const double y=std::max({a.y0-b.y1,b.y0-a.y1,0.0});
    return x*x+y*y;
  }
  size_t build(size_t begin,size_t end) {
    Box bounds=edges_[begin].box;
    for(size_t i=begin+1;i<end;++i) {
      const auto& b=edges_[i].box;
      bounds.x0=std::min(bounds.x0,b.x0);bounds.y0=std::min(bounds.y0,b.y0);
      bounds.x1=std::max(bounds.x1,b.x1);bounds.y1=std::max(bounds.y1,b.y1);
    }
    const size_t id=nodes_.size();nodes_.push_back({bounds,begin,end});
    if(end-begin>8) {
      const size_t mid=begin+(end-begin)/2;
      const bool x=bounds.x1-bounds.x0>=bounds.y1-bounds.y0;
      std::nth_element(edges_.begin()+begin,edges_.begin()+mid,edges_.begin()+end,[x](const Edge& a,const Edge& b) {
        return x ? a.box.x0+a.box.x1<b.box.x0+b.box.x1 : a.box.y0+a.box.y1<b.box.y0+b.box.y1;
      });
      const auto left=build(begin,mid),right=build(mid,end);
      nodes_[id].left=left;nodes_[id].right=right;
    }
    return id;
  }
  bool query(size_t id,const Edge& edge,double threshold) const {
    const auto& n=nodes_[id];
    if(distanceSquared(edge.box,n.box)>=threshold) return false;
    if(n.left) return query(n.left,edge,threshold)||query(n.right,edge,threshold);
    for(size_t i=n.begin;i<n.end;++i) {
      const auto& e=edges_[i];
      if(distanceSquared(edge.box,e.box)>=threshold) continue;
      if(segmentDistanceSquared(edge.a,edge.b,e.a,e.b)<threshold) return true;
    }
    return false;
  }
 public:
  explicit ContourIndex(const Polygon& p) {
    edges_.reserve(p.points.size());nodes_.reserve(p.points.size()*2);
    for(size_t i=0;i<p.points.size();++i) {
      const auto a=p.points[i],b=p.points[(i+1)%p.points.size()];
      edges_.push_back({a,b,box(a,b)});
    }
    if(!edges_.empty()) build(0,edges_.size());
  }
  bool tooClose(const ContourIndex& other,const Point& shift,double margin) const {
    if(margin<=0||edges_.empty()||other.edges_.empty()) return false;
    const auto& b=nodes_[0].box;
    const Box moved{b.x0+shift.x,b.y0+shift.y,b.x1+shift.x,b.y1+shift.y};
    if(distanceSquared(moved,other.nodes_[0].box)>=margin*margin) return false;
    const double m=std::max(0.0,margin-std::min(1e-7,margin*1e-9));
    const double threshold=m*m;
    for(const auto& e:edges_) {
      const Point a{e.a.x+shift.x,e.a.y+shift.y,true},b{e.b.x+shift.x,e.b.y+shift.y,true};
      if(other.query(0,{a,b,box(a,b)},threshold)) return true;
    }
    return false;
  }
};

// Exact rectangle geometry with a conservative numerical band for the fallback.
class RectangularSheetClearance {
  Bounds bounds_;
  bool enabled_{false};
 public:
  explicit RectangularSheetClearance(const Polygon& p):bounds_(getPolygonBounds(p.points)) {
    if(!p.children.empty()||p.points.size()!=4||bounds_.width<=0||bounds_.height<=0) return;
    // Four distinct box corners in perimeter order; reject diagonal/self-crossing edges.
    unsigned corners=0;
    for(size_t i=0;i<4;++i) {
      const auto& a=p.points[i];const auto& b=p.points[(i+1)%4];
      if(a.x!=b.x&&a.y!=b.y) return;
      const bool right=a.x!=bounds_.x,top=a.y!=bounds_.y;
      if((right&&a.x!=bounds_.x+bounds_.width)||(top&&a.y!=bounds_.y+bounds_.height)) return;
      corners|=1U<<((right?1:0)+(top?2:0));
    }
    enabled_=corners==15;
  }
  std::optional<bool> violation(const Bounds& part,double margin,double guard) const {
    if(!enabled_||margin<=guard) return std::nullopt;
    const double gap=std::min({part.x-bounds_.x,part.y-bounds_.y,
        bounds_.x+bounds_.width-part.x-part.width,bounds_.y+bounds_.height-part.y-part.height});
    if(gap<margin-guard) return true;
    if(gap>margin+guard) return false;
    return std::nullopt;
  }
};

// Owned by one refinement pass; rebuild only the part whose move was committed.
struct ClearanceIndex {
  ContourIndex outer;
  std::vector<ContourIndex> holes;
  explicit ClearanceIndex(const Polygon& p):outer(p) {
    holes.reserve(p.children.size());
    for(const auto& h:p.children) holes.emplace_back(h);
  }
  bool partViolation(const Polygon& moved,const Polygon& fixed,const ClearanceIndex& other,
                     const Point& shift,const Config& cfg) const {
    if(hasMaterialOverlap(moved,fixed,cfg)||outer.tooClose(other.outer,shift,cfg.spacing)) return true;
    for(const auto& h:holes) {
      if(h.tooClose(other.outer,shift,cfg.holeSpacing)) return true;
      for(const auto& k:other.holes) if(h.tooClose(k,shift,cfg.holeSpacing)) return true;
    }
    for(const auto& h:other.holes) if(outer.tooClose(h,shift,cfg.holeSpacing)) return true;
    return false;
  }
  bool sheetViolation(const Polygon& moved,const Polygon& fixed,const ClearanceIndex& other,
                      const Point& shift,const Config& cfg) const {
    if(hasMaterialOutsideSheet(moved,fixed,cfg)||outer.tooClose(other.outer,shift,cfg.sheetSpacing)) return true;
    for(const auto& h:other.holes) {
      if(outer.tooClose(h,shift,cfg.holeSpacing)) return true;
      for(const auto& k:holes) if(k.tooClose(h,shift,cfg.holeSpacing)) return true;
    }
    return false;
  }
};
} // namespace detail
} // namespace clinesting
