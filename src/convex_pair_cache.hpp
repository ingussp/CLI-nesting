#pragma once
#include "clinesting/geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace clinesting::detail {
// Convex NFPs are configuration-space obstacles: distance from B-A to A+(-B)
// equals the distance between the translated convex material regions.
// Holed/concave shapes and numerically ambiguous queries use the exact fallback.
class ConvexPairCache {
  struct Edge { Point a; double dx,dy,inverseLength; };
  struct Pair { std::vector<Edge> edges; Bounds bounds; };
  std::unordered_map<std::string,size_t> identities_;
  std::vector<std::vector<Point>> shapes_;
  std::unordered_map<uint64_t,Pair> pairs_;
 public:
  static constexpr size_t unsupported=std::numeric_limits<size_t>::max();
  size_t add(const Polygon& p) {
    if(!p.children.empty()||p.points.size()<3||p.points.size()>400) return unsupported;
    // Bit-exact keys: do not merge contours through rounded geometry identities.
    std::string key;key.reserve(p.points.size()*sizeof(double)*2);
    for(const auto& pt:p.points) {
      key.append(reinterpret_cast<const char*>(&pt.x),sizeof(double));
      key.append(reinterpret_cast<const char*>(&pt.y),sizeof(double));
    }
    if(auto it=identities_.find(key);it!=identities_.end()) return it->second;
    const auto hull=getHull(p.points);
    bool convex=hull.size()==p.points.size();
    const auto same=[](const Point& a,const Point& b){return a.x==b.x&&a.y==b.y;};
    if(convex) {
      size_t start=0;while(start<hull.size()&&!same(hull[start],p.points[0])) ++start;
      bool forward=true,backward=true;
      for(size_t i=0;i<hull.size();++i) {
        forward=forward&&same(p.points[i],hull[(start+i)%hull.size()]);
        backward=backward&&same(p.points[i],hull[(start+hull.size()-i)%hull.size()]);
      }
      convex=forward||backward;
    }
    if(!convex||shapes_.size()>=128) {identities_.emplace(std::move(key),unsupported);return unsupported;}
    const auto id=shapes_.size();shapes_.push_back(hull);identities_.emplace(std::move(key),id);return id;
  }
  std::optional<bool> violation(size_t a,size_t b,const Point& relative,double margin,double guard) {
    if(a==unsupported||b==unsupported||margin<=guard) return std::nullopt;
    const uint64_t key=(uint64_t(a)<<32)|b;
    auto it=pairs_.find(key);
    if(it==pairs_.end()) {
      if(pairs_.size()>=256) return std::nullopt;
      std::vector<Point> sums;sums.reserve(shapes_[a].size()*shapes_[b].size());
      for(const auto& p:shapes_[a]) for(const auto& q:shapes_[b]) sums.push_back({p.x-q.x,p.y-q.y,true});
      const auto hull=getHull(sums);
      if(hull.size()<3) return std::nullopt;
      Pair pair;pair.bounds=getPolygonBounds(hull);pair.edges.reserve(hull.size());
      for(size_t i=0;i<hull.size();++i) {
        const auto& p=hull[i];const auto& q=hull[(i+1)%hull.size()];
        const double dx=q.x-p.x,dy=q.y-p.y;
        pair.edges.push_back({p,dx,dy,1/(dx*dx+dy*dy)});
      }
      it=pairs_.emplace(key,std::move(pair)).first;
    }
    const auto& pair=it->second;const auto& box=pair.bounds;
    const double hi=(margin+guard)*(margin+guard),lo=(margin-guard)*(margin-guard);
    const double bx=std::max({box.x-relative.x,relative.x-box.x-box.width,0.0});
    const double by=std::max({box.y-relative.y,relative.y-box.y-box.height,0.0});
    if(bx*bx+by*by>hi) return false;
    bool inside=true;double best=std::numeric_limits<double>::infinity();
    for(const auto& e:pair.edges) {
      const double x=relative.x-e.a.x,y=relative.y-e.a.y;
      if(e.dx*y-e.dy*x<0) inside=false;
      const double t=std::clamp((x*e.dx+y*e.dy)*e.inverseLength,0.0,1.0);
      const double dx=x-t*e.dx,dy=y-t*e.dy;
      best=std::min(best,dx*dx+dy*dy);
      if(best<lo) return true;
    }
    if(inside) return true;
    if(best>hi) return false;
    return std::nullopt;
  }
};
} // namespace clinesting::detail
