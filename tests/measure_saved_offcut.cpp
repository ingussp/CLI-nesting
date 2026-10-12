#include "clinesting/json_io.hpp"
#include "clinesting/reusable_offcut.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace clinesting;
int main(int argc,char** argv) {
 try {
  if(argc<4 || argc>5) throw std::runtime_error("Expected input.json result.json output.json [repeats]");
  const auto input=readNestingJson(argv[1]);auto json=nlohmann::json::parse(std::ifstream(argv[2]));
  PlacementResult result;
  for(const auto& sheet:json.at("sheets")) {
    SheetPlacement placed;placed.sheetid=sheet.at("id").get<int>();
    for(const auto& part:sheet.at("parts")) {
      Placement p;p.id=part.at("id").get<int>();p.x=part.at("x");p.y=part.at("y");p.rotation=part.at("rotation");placed.sheetplacements.push_back(p);
    }
    result.placements.push_back(placed);
  }
  const int repeats=argc>4 ? std::stoi(argv[4]) : 1;double ms=0;ReusableOffcut offcut;
  if(repeats<1) throw std::invalid_argument("repeats must be positive");
  for(int i=0;i<repeats;++i) {offcut=measureReusableOffcut(input.sheets,input.individual.placement,result,input.config);ms+=offcut.elapsedMs;}
  nlohmann::json j={{"area",offcut.area},{"freeArea",offcut.freeArea},{"coreArea",offcut.coreArea},
      {"minWidthMm",offcut.minWidthMm},{"components",offcut.components},{"averageMs",ms/repeats},{"backend","cpu-vector"}};
  j["availableArea"]=offcut.availableArea;j["clearanceExcludedArea"]=offcut.clearanceExcludedArea;
  j["arcToleranceMm"]=offcut.arcToleranceMm;
  j["clearances"]={{"spacing",offcut.spacingMm},{"partToSheet",offcut.sheetSpacingMm},{"partToHole",offcut.holeSpacingMm}};
  auto points=[](const Polygon& p) {auto v=nlohmann::json::array();for(const auto& x:p.points) v.push_back({x.x,x.y});return v;};
  j["points"]=points(offcut.contour);j["holes"]=nlohmann::json::array();
  for(const auto& hole:offcut.contour.children) j["holes"].push_back(points(hole));
  if(offcut.sheetId) j["sheetId"]=*offcut.sheetId;
  json["reusableOffcut"]=j;std::ofstream(argv[3])<<json.dump(2);
  j.erase("points");j.erase("holes");std::cout<<j.dump()<<"\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
