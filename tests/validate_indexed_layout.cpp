#include "clinesting/json_io.hpp"
#include "clinesting/geometry.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace clinesting;
using Json=nlohmann::json;
Polygon contour(const Json& points) {
  Polygon p;for(const auto& pt:points) p.points.push_back({pt[0].get<double>(),pt[1].get<double>(),true});return p;
}
Polygon polygon(const Json& j) {
  auto p=contour(j.at("points"));
  for(const auto& hole:j.at("holes")) p.children.push_back(contour(hole));return p;
}
int main(int argc,char** argv) {
  try {
    if(argc!=3) throw std::runtime_error("Expected input.json result.json");
    const auto request=readNestingJson(argv[1]);
    const auto result=Json::parse(std::ifstream(argv[2]));
    size_t placed=0,neighbours=0;
    for(const auto& s:result.at("sheets")) {
      const auto sheet=polygon(s);
      std::vector<Polygon> parts;std::vector<Bounds> bounds;
      for(const auto& p:s.at("parts")) {parts.push_back(polygon(p));bounds.push_back(getPolygonBounds(parts.back().points));}
      for(size_t i=0;i<parts.size();++i) {
        if(violatesSheetClearance(parts[i],sheet,request.config)) throw std::runtime_error("Sheet clearance violation");
        for(size_t j=0;j<i;++j) {
          const auto& a=bounds[i];const auto& b=bounds[j];
          const double dx=std::max({a.x-b.x-b.width,b.x-a.x-a.width,0.0});
          const double dy=std::max({a.y-b.y-b.height,b.y-a.y-a.height,0.0});
          const double gap=std::max(request.config.spacing,request.config.holeSpacing);
          if(dx*dx+dy*dy>gap*gap) continue;
          ++neighbours;
          // Deliberately use the unchanged exhaustive reference predicates.
          if(violatesPartClearance(parts[i],parts[j],request.config)) throw std::runtime_error("Part clearance violation");
        }
        ++placed;
      }
    }
    if(placed!=result.at("placed").get<size_t>()) throw std::runtime_error("Placement count mismatch");
    std::cout<<placed<<" parts validated by original exhaustive predicates; "<<neighbours<<" nearby pairs; no overlap or clearance violations\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
