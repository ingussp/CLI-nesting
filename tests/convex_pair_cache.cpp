#include "convex_pair_cache.hpp"
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>
using namespace clinesting;
Polygon shape(int n,double radius) {
  Polygon p;for(int i=0;i<n;++i) {double a=2*std::numbers::pi*i/n;p.points.push_back({radius*std::cos(a),radius*std::sin(a),true});}return p;
}
int main() {
  try {
    std::mt19937_64 rng(194950);std::uniform_real_distribution<double> distance(-120,120),angle(0,360);
    size_t answered=0,fallback=0;
    for(int i=0;i<400;++i) {
      auto a=rotatePolygon(shape(i%5==0?148:4+i%31,20+i%19),angle(rng));
      auto b=rotatePolygon(shape(i%7==0?148:4+i%29,20+i%23),angle(rng));
      detail::ConvexPairCache cache;auto ai=cache.add(a),bi=cache.add(b);
      if(ai==cache.unsupported||bi==cache.unsupported||cache.add(a)!=ai) throw std::runtime_error("Convex shape registration failed");
      Config cfg;cfg.spacing=6.1;
      for(int k=0;k<20;++k) {
        const Point originA{i%2?1e6:0.0,i%2?-1e6:0.0,true};
        const Point relative{distance(rng),distance(rng),true};
        const Point originB{originA.x+relative.x,originA.y+relative.y,true};
        const auto answer=cache.violation(ai,bi,relative,cfg.spacing,1e-5);
        if(answer) {
          if(*answer!=violatesPartClearance(shiftPolygon(a,originA),shiftPolygon(b,originB),cfg)) throw std::runtime_error("Convex pair answer differs from exhaustive reference");
          ++answered;
        } else ++fallback;
      }
    }
    const auto a=shape(148,50);
    detail::ConvexPairCache cache;const auto id=cache.add(a);
    Config cfg;cfg.spacing=6.1;
    for(double e:{-1e-4,-1e-5,-1e-7,0.0,1e-7,1e-5,1e-4}) {
      const Point relative{100+cfg.spacing+e,0,true};
      auto answer=cache.violation(id,id,relative,cfg.spacing,1e-5);
      if(answer) {
        if(*answer!=violatesPartClearance(a,shiftPolygon(a,relative),cfg)) throw std::runtime_error("Boundary mismatch");
        ++answered;
      } else ++fallback;
    }
    auto holed=a;holed.children.push_back(shape(8,10));
    if(cache.add(holed)!=cache.unsupported) throw std::runtime_error("Holed shape must use fallback");
    Polygon concave;concave.points={{0,0},{10,0},{10,10},{5,5},{0,10}};
    if(cache.add(concave)!=cache.unsupported) throw std::runtime_error("Concave shape must use fallback");
    if(cache.violation(id,id,{100,0},0,1e-5)) throw std::runtime_error("Zero spacing must use fallback");
    if(answered<7900||fallback==0) throw std::runtime_error("Insufficient fast/fallback coverage");
    std::cout<<answered<<" convex pair answers matched exhaustive reference; "<<fallback<<" boundary fallbacks verified\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
