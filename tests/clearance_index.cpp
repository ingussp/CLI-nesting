#include "clearance_index.hpp"
#include <iostream>
#include <random>
#include <numbers>
#include <stdexcept>
using namespace clinesting;
Polygon contour(int count,double radius,bool concave=false) {
  Polygon p;
  for(int i=0;i<count;++i) {
    const double a=2*std::numbers::pi*i/count;
    const double r=concave && i%2 ? radius*0.55 : radius;
    p.points.push_back({std::cos(a)*r,std::sin(a)*r,true});
  }
  return p;
}
int main() {
  try {
    std::mt19937_64 random(42910450);
    std::uniform_real_distribution<double> pos(-150,150),angle(0,360);
    size_t cases=0;
    for(int n=0;n<1200;++n) {
      auto a=rotatePolygon(contour(n%3==0?148:4+n%29,20+n%41,n%2),angle(random));
      auto b=rotatePolygon(contour(n%4==0?148:4+n%37,20+n%43,n%3),angle(random));
      if(n%3==0) a.children.push_back(contour(17,8));
      if(n%5==0) b.children.push_back(contour(23,9));
      if(n%7==0) b.children.push_back(shiftPolygon(contour(11,3),{15,0,true}));
      // Cover absolute world offsets, zero margins, overlap, holes, and translations.
      const Point world{n%2 ? 1e6 : -123.5,n%2 ? -1e6 : 87.25,true};
      a=shiftPolygon(a,world);b=shiftPolygon(b,world);
      const detail::ClearanceIndex ai(a),bi(b);
      Config cfg;cfg.spacing=n%7==0?0:6.1;cfg.holeSpacing=2.25;cfg.sheetSpacing=10;
      for(int k=0;k<4;++k) {
        const Point shift{pos(random),pos(random),true};
        const auto moved=shiftPolygon(a,shift);
        if(ai.partViolation(moved,b,bi,shift,cfg)!=violatesPartClearance(moved,b,cfg))
          throw std::runtime_error("Indexed part clearance differs from reference");
        if(ai.sheetViolation(moved,b,bi,shift,cfg)!=violatesSheetClearance(moved,b,cfg))
          throw std::runtime_error("Indexed sheet clearance differs from reference");
        cases+=2;
      }
    }
    Polygon square;square.points={{0,0},{10,0},{10,10},{0,10}};
    const detail::ClearanceIndex si(square);
    for(double margin:{0.0,0.001,2.25,6.1,6.5,10.0}) for(double epsilon:{-1e-6,-1e-8,-1e-10,0.0,1e-10,1e-8,1e-6}) {
      Config cfg;cfg.spacing=margin;
      Point shift{10+margin+epsilon,0,true};
      const auto moved=shiftPolygon(square,shift);
      if(si.partViolation(moved,square,si,shift,cfg)!=violatesPartClearance(moved,square,cfg))
        throw std::runtime_error("Indexed threshold differs from reference");
      ++cases;
    }
    Polygon stock;stock.points={{-100,-200},{300,-200},{300,400},{-100,400}};
    detail::RectangularSheetClearance rectangle(stock);
    size_t rectangleAnswers=0;
    for(int i=0;i<3000;++i) {
      const auto p=shiftPolygon(rotatePolygon(contour(31,25,i%2),angle(random)),{pos(random),pos(random),true});
      Config cfg;cfg.sheetSpacing=10;
      const auto answer=rectangle.violation(getPolygonBounds(p.points),cfg.sheetSpacing,1e-5);
      if(answer) {
        if(*answer!=violatesSheetClearance(p,stock,cfg)) throw std::runtime_error("Rectangle margin differs from reference");
        ++rectangleAnswers;
      }
    }
    for(double e:{-1e-4,-1e-8,0.0,1e-8,1e-4}) {
      const auto p=shiftPolygon(square,{-90+e,0,true});Config cfg;cfg.sheetSpacing=10;
      const auto answer=rectangle.violation(getPolygonBounds(p.points),10,1e-5);
      if(answer&&*answer!=violatesSheetClearance(p,stock,cfg)) throw std::runtime_error("Rectangle threshold mismatch");
      if(std::abs(e)<1e-5&&answer) throw std::runtime_error("Rectangle threshold should use exact fallback");
    }
    stock.children.push_back(square);
    if(detail::RectangularSheetClearance(stock).violation(getPolygonBounds(square.points),10,1e-5))
      throw std::runtime_error("Stock cutouts must use exact fallback");
    std::cout<<rectangleAnswers<<" rectangular stock answers matched reference\n";
    std::cout<<cases<<" indexed/reference clearance comparisons passed\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
