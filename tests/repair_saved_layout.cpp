#include "clinesting/json_io.hpp"
#include "clinesting/continuous_nesting.hpp"
#include "clinesting/geometry.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace clinesting;
int main(int argc,char** argv) {
 try {
  if(argc!=4) throw std::runtime_error("Expected input.json incumbent.json output.json");
  auto request=readNestingJson(argv[1]);const auto saved=nlohmann::json::parse(std::ifstream(argv[2]));
  PlacementResult best;BitmapNestingStats bestStats;bestStats.occupiedBoundsArea=saved.at("occupiedBoundsArea");
  std::vector<int> ids;
  for(const auto& sheet:saved.at("sheets")) {
   SheetPlacement layout;layout.sheetid=sheet.at("id").get<int>();
   for(const auto& p:sheet.at("parts")) {
    Placement placement;placement.id=p.at("id").get<int>();placement.x=p.at("x");placement.y=p.at("y");placement.rotation=p.at("rotation");
    placement.source=p.value("source",std::string{});layout.sheetplacements.push_back(placement);ids.push_back(*placement.id);
   }
   best.placements.push_back(layout);
   if(!layout.sheetplacements.empty()) {
    const auto it=std::find_if(request.sheets.begin(),request.sheets.end(),[&](const auto& p){return p.id==layout.sheetid;});
    best.totalarea+=polygonMaterialArea(*it);
   }
  }
  for(const auto& part:request.individual.placement) {
   if(std::find(ids.begin(),ids.end(),*part.id)==ids.end()) best.unplaced.push_back(part);
   else best.area+=polygonMaterialArea(part);
  }
  for(int i=0;i<160;++i) {
   auto cfg=request.config;cfg.timeLimitSeconds=2;cfg.searchIteration=i;
   BitmapNestingStats stats;auto candidate=refillBitmapLayout(request.sheets,request.individual.placement,best,cfg,&stats);
   std::cout<<"pass="<<i<<" placed="<<request.individual.placement.size()-candidate.unplaced.size()<<" moves="<<stats.vectorMoves<<" refill="<<stats.refillPlacements<<" ms="<<stats.totalBitmapMs<<std::endl;
   if(improvesLayout(layoutQuality(request.sheets,candidate,stats),layoutQuality(request.sheets,best,bestStats))) {best=std::move(candidate);bestStats=stats;}
   OrchestratorRunStats out;out.placement=best;out.bitmapStats=bestStats;writeNestingJson(argv[3],request,out);
  }
  return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
