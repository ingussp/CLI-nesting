#pragma once
#include "clinesting/model.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace clinesting::detail {
// Emit half-open pixel spans with the same even-odd, pixel-center convention
// as point-in-polygon sampling. Reuse crossings across rows and contours.
template<class Emit>
void rasterContourRow(const std::vector<Point>& points,double y,double originX,
                      double resolution,int width,std::vector<double>& crossings,Emit emit) {
  crossings.clear();
  if(points.size()<3) return;
  for(size_t i=0,j=points.size()-1;i<points.size();j=i++) {
    const auto& a=points[i]; const auto& b=points[j];
    if((a.y>y)!=(b.y>y))
      crossings.push_back((b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x);
  }
  std::sort(crossings.begin(),crossings.end());
  for(size_t i=0;i+1<crossings.size();i+=2) {
    const auto pixel=[&](double x) {
      return int(std::clamp(std::ceil((x-originX)/resolution-0.5),0.0,double(width)));
    };
    emit(pixel(crossings[i]),pixel(crossings[i+1]));
  }
}
}
