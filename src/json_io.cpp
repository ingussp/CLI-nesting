#include "clinesting/json_io.hpp"
#include "clinesting/continuous_nesting.hpp"
#include "clinesting/geometry.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace clinesting {
namespace {
using Json=nlohmann::json;
// Read a finite JSON number within the supported coordinate magnitude.
double number(const Json& v,const std::string& name) {
  if(!v.is_number()) throw std::invalid_argument(name+" must be a number");
  const double n=v.get<double>();
  if(!std::isfinite(n) || std::abs(n)>1000000) throw std::invalid_argument(name+" must be finite and within +/-1000000");
  return n;
}
// Read a JSON integer within the requested bounds.
int integer(const Json& v,const std::string& name,int low,int high) {
  const double n=number(v,name);
  if(std::floor(n)!=n || n<low || n>high) throw std::invalid_argument(name+" is outside the supported integer range");
  return int(n);
}
// Choose an explicit ID, fallback name or generated identifier.
std::string identifier(const Json& obj,const std::string& fallback) {
  if(!obj.contains("id")) return obj.value("name",fallback);
  const auto& id=obj["id"];
  if(id.is_string()) return id.get<std::string>();
  if(id.is_number_integer()) return id.dump();
  throw std::invalid_argument("id must be a string or integer");
}
// Parse and normalize an ordered nonzero-area point contour.
Polygon contour(const Json& input,const std::string& name) {
  const auto& points=input.is_object() ? input.at(input.contains("points") ? "points" : "outer") : input;
  if(!points.is_array() || points.size()<3 || points.size()>20000)
    throw std::invalid_argument(name+" requires 3..20000 points");
  Polygon p;
  for(const auto& item:points) {
    Point v;
    if(item.is_array() && (item.size()==2 || item.size()==3)) {
      v={number(item[0],name+".x"),number(item[1],name+".y"),true};
      if(item.size()==3) static_cast<void>(number(item[2],name+".z"));
    } else if(item.is_object() && item.contains("x") && item.contains("y")) {
      v={number(item["x"],name+".x"),number(item["y"],name+".y"),true};
    } else throw std::invalid_argument(name+" points must be [x,y] or {x,y}");
    if(p.points.empty() || p.points.back().x!=v.x || p.points.back().y!=v.y) p.points.push_back(v);
  }
  if(p.points.size()>1 && p.points.front().x==p.points.back().x && p.points.front().y==p.points.back().y) p.points.pop_back();
  if(p.points.size()<3 || std::abs(polygonArea(p))<1e-9) throw std::invalid_argument(name+" has zero area");
  return p;
}
// Parse an outline, holes, metadata and per-part orientation rules.
Polygon polygon(const Json& obj,const std::string& name,bool sheet) {
  if(!obj.is_object()) throw std::invalid_argument(name+" must be an object");
  Polygon p;
  if(obj.contains("points") && obj.contains("outer") && obj["points"]!=obj["outer"])
    throw std::invalid_argument(name+": points and outer disagree");
  if(obj.contains("points") || obj.contains("outer")) p=contour(obj,name);
  else if(sheet && obj.contains("width") && obj.contains("height")) {
    const double w=number(obj["width"],name+".width"),h=number(obj["height"],name+".height");
    if(w<=0 || h<=0) throw std::invalid_argument("Sheet dimensions must be positive");
    const double x=obj.contains("x") ? number(obj["x"],"sheet.x") : 0;
    const double y=obj.contains("y") ? number(obj["y"],"sheet.y") : 0;
    p.points={{x,y,true},{x+w,y,true},{x+w,y+h,true},{x,y+h,true}};
  } else throw std::invalid_argument(name+" is missing points");
  if(obj.contains("holes")) {
    if(!obj["holes"].is_array()) throw std::invalid_argument(name+".holes must be an array");
    for(const auto& hole:obj["holes"]) {
      auto h=contour(hole,name+".hole");
      Polygon outer=p; outer.children.clear();
      if(hasMaterialOutsideSheet(h,outer,Config{})) throw std::invalid_argument(name+" hole is outside its contour");
      for(const auto& previous:p.children) if(hasMaterialOverlap(h,previous,Config{}))
        throw std::invalid_argument(name+" holes overlap");
      p.children.push_back(std::move(h));
    }
  }
  if(polygonMaterialArea(p)<=0) throw std::invalid_argument(name+" has no material area");
  p.source=identifier(obj,name);
  p.filename=obj.value("filename",std::string{});
  if(obj.contains("_ip_nesting")) p.metadataJson=obj["_ip_nesting"].dump();
  if(obj.contains("rotation")) p.rotation=number(obj["rotation"],name+".rotation");
  if(obj.contains("angle") || obj.contains("allowedAngles")) {
    if(sheet) throw std::invalid_argument("angle/allowedAngles apply to parts, not sheets");
    if(obj.contains("angle") && obj.contains("allowedAngles"))
      throw std::invalid_argument(name+": use angle or allowedAngles, not both");
    if(p.rotation!=0) throw std::invalid_argument(name+": absolute angles cannot be combined with a nonzero rotation offset");
    const Json angles=obj.contains("angle") ? Json::array({obj["angle"]}) : obj["allowedAngles"];
    if(!angles.is_array() || angles.empty() || angles.size()>Config::maxRotations)
      throw std::invalid_argument(name+".allowedAngles requires 1..3600 angles");
    for(const auto& value:angles) {
      double angle=std::fmod(number(value,name+".angle"),360.0);
      if(angle<0) angle+=360.0;
      if(angle==0) angle=0; // canonicalize negative zero
      p.allowedAngles.push_back(angle);
    }
    std::sort(p.allowedAngles.begin(),p.allowedAngles.end());
    p.allowedAngles.erase(std::unique(p.allowedAngles.begin(),p.allowedAngles.end()),p.allowedAngles.end());
  }
  if(obj.contains("rotations")) {
    if(sheet) throw std::invalid_argument("Per-object rotations apply to parts, not sheets");
    if(!p.allowedAngles.empty()) throw std::invalid_argument(name+": use rotations or absolute angles, not both");
    const int count=integer(obj["rotations"],name+".rotations",1,Config::maxRotations);
    for(int i=0;i<count;++i) {
      double angle=std::fmod(p.rotation+360.0*i/count,360.0);
      if(angle<0) angle+=360.0;
      p.allowedAngles.push_back(angle);
    }
  }
  // Calculate identity from the complete geometry. Never trust a user supplied
  // name or id as a geometry cache key.
  std::ostringstream identity;
  identity<<std::setprecision(17);
  auto append=[&](const Polygon& ring) {
    identity<<'[';
    for(const auto& v:ring.points) identity<<v.x<<','<<v.y<<';';
    identity<<']';
  };
  append(p);
  for(const auto& hole:p.children) append(hole);
  p.geometryKey=identity.str();
  return p;
}
// Validate and apply the merged CLI search options.
void configure(const Json& j,Config& c) {
  if(!j.is_object()) throw std::invalid_argument("config must be an object");
  for(auto it=j.begin();it!=j.end();++it) {
    const auto& k=it.key(); const auto& v=it.value();
    if(k=="rotations") c.rotations=integer(v,k,1,Config::maxRotations);
    else if(k=="rotationStep") {
      const double step=number(v,k);
      if(step<360.0/Config::maxRotations || step>360.0)
        throw std::invalid_argument("rotationStep must be between 0.1 and 360 degrees");
      const double count=360.0/step;
      const int rotations=static_cast<int>(std::round(count));
      if(std::abs(count-rotations)>1e-8)
        throw std::invalid_argument("rotationStep must divide 360 degrees into a whole number of rotations");
      if(j.contains("rotations") && integer(j["rotations"],"rotations",1,Config::maxRotations)!=rotations)
        throw std::invalid_argument("rotations and rotationStep describe different angle grids");
      c.rotations=rotations;
    }
    else if(k=="threads") c.threads=integer(v,k,1,256);
    else if(k=="trials") c.bitmapTrials=integer(v,k,1,4);
    else if(k=="mode") {
      if(v=="first") c.mode=SearchMode::First;
      else if(v=="timed") c.mode=SearchMode::Timed;
      else if(v=="continuous") c.mode=SearchMode::Continuous;
      else throw std::invalid_argument("mode must be first, timed or continuous");
    }
    else if(k=="continuous") {
      if(!v.is_boolean()) throw std::invalid_argument("continuous must be boolean");
      c.continuous=v.get<bool>();
    }
    else if(k=="continuousRoundSeconds") {
      c.continuousRoundSeconds=number(v,k);
      if(c.continuousRoundSeconds<0.01 || c.continuousRoundSeconds>86400)
        throw std::invalid_argument("continuousRoundSeconds must be between 0.01 and 86400");
    }
    else if(k=="timeLimitSeconds") {
      c.timeLimitSeconds=number(v,k);
      if(c.timeLimitSeconds<0 || c.timeLimitSeconds>86400)
        throw std::invalid_argument("timeLimitSeconds must be between 0 and 86400");
    }
    else if(k=="gpu") {
      if(v.is_boolean()) c.gpuEnabled=v.get<bool>();
      else if(v.is_object()) {
        c.gpuEnabled=true;
        for(auto option=v.begin();option!=v.end();++option) {
          const auto& key=option.key(); const auto& val=option.value();
          if(key=="enabled" || key=="fallbackToCpu") {
            if(!val.is_boolean()) throw std::invalid_argument("gpu."+key+" must be boolean");
            if(key=="enabled") c.gpuEnabled=val.get<bool>(); else c.gpuFallbackToCpu=val.get<bool>();
          } else if(key=="device") c.gpuDevice=integer(val,"gpu.device",-1,1024);
          else if(key=="batchSize") c.gpuBatchSize=integer(val,"gpu.batchSize",256,262144);
          else throw std::invalid_argument("Unknown gpu option: "+key);
        }
      } else throw std::invalid_argument("gpu must be boolean or an object");
    }
    else if(k=="bitmapResolutionMm" || k=="resolution") {
      c.bitmapResolutionMm=number(v,k); if(c.bitmapResolutionMm<=0) throw std::invalid_argument(k+" must be positive");
    } else if(k=="bitmapSearchStepPx" || k=="step") c.bitmapSearchStepPx=integer(v,k,1,100000);
    else if(k=="curveTolerance") { c.curveTolerance=number(v,k); if(c.curveTolerance<0) throw std::invalid_argument(k+" must be nonnegative"); }
    else if(k=="cacheRejects") { if(!v.is_boolean()) throw std::invalid_argument(k+" must be boolean"); c.bitmapCacheRejects=v.get<bool>(); }
    else if(k=="algorithm") {
      if(v=="bitmap") c.algorithm=NestingAlgorithm::Bitmap;
      else if(v=="nfp") c.algorithm=NestingAlgorithm::Nfp;
      else throw std::invalid_argument("algorithm must be bitmap or nfp");
    } else if(k=="spacing" || k=="partToSheet" || k=="partToHole") {
      const double gap=number(v,k);
      if(gap<0) throw std::invalid_argument(k+" must be nonnegative");
      if(k=="spacing") c.spacing=gap;
      else if(k=="partToSheet") c.sheetSpacing=gap;
      else c.holeSpacing=gap;
    } else throw std::invalid_argument("Unknown config field: "+k);
  }
  if(!j.contains("mode")) c.mode=c.continuous ? SearchMode::Continuous :
    c.timeLimitSeconds>0 ? SearchMode::Timed : SearchMode::First;
  if(j.contains("mode") && j.contains("continuous") && c.continuous!=(c.mode==SearchMode::Continuous))
    throw std::invalid_argument("mode and legacy continuous disagree");
  c.continuous=c.mode==SearchMode::Continuous;
  if(c.mode==SearchMode::Timed && c.timeLimitSeconds<=0)
    throw std::invalid_argument("timed mode requires positive timeLimitSeconds");
  if(j.contains("mode") && c.mode==SearchMode::First && c.timeLimitSeconds>0)
    throw std::invalid_argument("first mode requires timeLimitSeconds=0; use timed for a time budget");
}
// Serialize a contour as arrays of XY coordinates.
Json pointsJson(const Polygon& p) {
  Json points=Json::array();
  for(const auto& v:p.points) points.push_back({v.x,v.y});
  return points;
}
// Serialize all child contours as hole point arrays.
Json holesJson(const Polygon& p) {
  Json holes=Json::array(); for(const auto& h:p.children) holes.push_back(pointsJson(h)); return holes;
}
}

// Merge FreeCAD settings and explicit CLI overrides while discarding only known legacy options.
Json inputConfig(const Json& root) {
  Json merged=Json::object();
  const std::unordered_set<std::string> legacy={"placementType","simplify","useSvgPreProcessor","scale",
    "endpointTolerance","dxfImportScale","dxfExportScale","exportWithSheetBoundboarders",
    "exportWithSheetsSpace","exportWithSheetsSpaceValue","mergeLines","timeRatio",
    "populationSize","mutationRate","useQuantityFromFileName"};
  for(const char* field:{"settings","config","CLI-nesting"}) {
    if(!root.contains(field)) continue;
    const auto& block=root[field];
    if(!block.is_object()) throw std::invalid_argument(std::string(field)+" must be an object");
    for(auto it=block.begin();it!=block.end();++it) {
      if(std::string_view(field)=="settings" && (it.key()=="units" || legacy.contains(it.key()))) continue;
      const auto key=it.key()=="sheetSpacing" ? "partToSheet" : it.key()=="holeSpacing" ? "partToHole" : it.key();
      if(key!=it.key() && block.contains(key) && block[key]!=it.value())
        throw std::invalid_argument(std::string(field)+": conflicting clearance aliases");
      merged[key]=it.value();
    }
  }
  return merged;
}

// Parse FreeCAD or CLI JSON and validate the complete job.
BackgroundRequest parseNestingJson(std::string_view text) {
  try {
    const auto root=Json::parse(text);
    if(!root.is_object()) throw std::invalid_argument("JSON root must be an object");
    if(root.contains("units") && root["units"]!="mm") throw std::invalid_argument("Only millimetres (units: mm) are supported");
    if(root.contains("schema_version") && integer(root["schema_version"],"schema_version",1,1)!=1)
      throw std::invalid_argument("Only schema_version 1 is supported");
    for(const char* field:{"settings","_ip_nesting"}) {
      if(root.contains(field) && root[field].is_object() && root[field].contains("units") && root[field]["units"]!="mm")
        throw std::invalid_argument(std::string(field)+".units must be mm");
    }
    BackgroundRequest request;
    request.jobId=root.value("job_id",std::string{});
    request.createdAt=root.value("created_at",std::string{});
    if(root.contains("_ip_nesting")) request.metadataJson=root["_ip_nesting"].dump();
    request.config.algorithm=NestingAlgorithm::Bitmap;
    request.config.placementType="box";
    configure(inputConfig(root),request.config);
    if(root.contains("output")) {
      const auto& output=root["output"];
      if(!output.is_object()) throw std::invalid_argument("output must be an object");
      if(output.contains("json") && output.contains("resultJson") && output["json"]!=output["resultJson"])
        throw std::invalid_argument("output.json and output.resultJson disagree");
      for(auto it=output.begin();it!=output.end();++it) {
        const auto& key=it.key(); const auto& value=it.value();
        auto pathValue=[&]() {
          if(!value.is_string()) throw std::invalid_argument("output."+key+" must be a path string");
          auto path=value.get<std::string>();
          if(path.empty() || path.find('\0')!=std::string::npos) throw std::invalid_argument("Invalid output path");
          return path;
        };
        if(key=="json" || key=="resultJson") request.output.json=pathValue();
        else if(key=="dxf" || key=="svg") {
          auto& path=key=="dxf" ? request.output.dxf : request.output.svg;
          path=value.is_boolean() ? (value.get<bool>() ? "result."+key : "") : pathValue();
        } else if(key=="openPreview") {
          if(!value.is_boolean()) throw std::invalid_argument("output.openPreview must be boolean");
          request.output.openPreview=value.get<bool>();
        } else throw std::invalid_argument("Unknown output option: "+key);
      }
      if(request.output.openPreview && request.output.svg.empty())
        throw std::invalid_argument("output.openPreview requires output.svg");
    }
    if(request.config.mode!=SearchMode::First && request.config.algorithm!=NestingAlgorithm::Bitmap)
      throw std::invalid_argument("timed/continuous modes require algorithm: bitmap");
    if(request.config.continuous && request.config.algorithm!=NestingAlgorithm::Bitmap)
      throw std::invalid_argument("Continuous search requires algorithm: bitmap");
    if(request.config.gpuEnabled && request.config.algorithm!=NestingAlgorithm::Bitmap)
      throw std::invalid_argument("GPU acceleration requires algorithm: bitmap");
    if(request.config.algorithm!=NestingAlgorithm::Bitmap &&
       (request.config.spacing>0 || request.config.sheetSpacing>0 || request.config.holeSpacing>0))
      throw std::invalid_argument("Clearance settings require algorithm: bitmap");
    const auto& parts=root.at("parts");
    if(!parts.is_array() || parts.empty()) throw std::invalid_argument("parts must be a nonempty array");
    int instanceId=1;
    std::unordered_set<std::string> sources;
    for(size_t i=0;i<parts.size();++i) {
      auto p=polygon(parts[i],"part_"+std::to_string(i+1),false);
      if(!sources.insert(p.source).second) throw std::invalid_argument("Duplicate part id: "+p.source);
      int count=1;
      if(parts[i].contains("quantity")) count=integer(parts[i]["quantity"],"quantity",1,100000);
      else if(parts[i].contains("count")) count=integer(parts[i]["count"],"count",1,100000);
      if(request.individual.placement.size()+size_t(count)>100000) throw std::invalid_argument("At most 100000 part instances are supported");
      for(int n=0;n<count;++n) { p.id=instanceId++; request.individual.placement.push_back(p); }
    }
    Json sheets=root.contains("sheets") ? root["sheets"] : Json::array({root.at("sheet")});
    if(!sheets.is_array() || sheets.empty()) throw std::invalid_argument("sheets must be a nonempty array");
    int sheetId=1;
    for(size_t i=0;i<sheets.size();++i) {
      auto p=polygon(sheets[i],"sheet_"+std::to_string(i+1),true);
      const int count=sheets[i].contains("quantity") ? integer(sheets[i]["quantity"],"sheet quantity",1,1000) : 1;
      if(request.sheets.size()+size_t(count)>1000) throw std::invalid_argument("At most 1000 sheets are supported");
      for(int n=0;n<count;++n) { p.id=sheetId++; request.sheets.push_back(p); }
    }
    if(request.config.algorithm!=NestingAlgorithm::Bitmap &&
       (request.config.timeLimitSeconds>0 || std::any_of(request.individual.placement.begin(),request.individual.placement.end(),
         [](const Polygon& p) { return !p.allowedAngles.empty(); })))
      throw std::invalid_argument("Per-part angles and time limits require algorithm: bitmap");
    return request;
  } catch(const Json::exception& e) { throw std::invalid_argument(std::string("Invalid nesting JSON: ")+e.what()); }
}

// Read a size-limited JSON input file and parse the job.
BackgroundRequest readNestingJson(const std::filesystem::path& path) {
  if(std::filesystem::file_size(path)>64ULL*1024*1024) throw std::invalid_argument("JSON input exceeds 64 MiB");
  std::ifstream stream(path,std::ios::binary);
  if(!stream) throw std::runtime_error("Cannot open input JSON: "+path.string());
  const std::string text((std::istreambuf_iterator<char>(stream)),{});
  return parseNestingJson(text);
}

// Export transformed contours, metadata and search diagnostics as JSON.
void writeNestingJson(const std::filesystem::path& path,const BackgroundRequest& input,const OrchestratorRunStats& run) {
  const auto& r=run.placement;
  Json out={{"schemaVersion",1},{"units","mm"},{"placed",input.individual.placement.size()-r.unplaced.size()},
    {"rotations",input.config.rotations},{"rotationStep",360.0/input.config.rotations},
    {"unplacedCount",r.unplaced.size()},{"utilisation",r.utilisation},{"timingMs",run.timings.totalMs},
    {"trials",run.bitmapStats.completedTrials},{"workersUsed",run.bitmapStats.workersUsed},
    {"proposalWorkersPerTrial",run.bitmapStats.proposalWorkersPerTrial},
    {"patternPlacements",run.bitmapStats.patternPlacements},{"rejectedPositionSkips",run.bitmapStats.rejectedPositionSkips},
    {"sheets",Json::array()},{"unplaced",Json::array()}};
  out["selectedTrial"]=run.bitmapStats.selectedTrial;
  out["clearances"]={{"spacing",input.config.spacing},{"partToSheet",input.config.sheetSpacing},
                     {"partToHole",input.config.holeSpacing}};
  out["schema_version"]=1;
  if(!input.jobId.empty()) out["job_id"]=input.jobId;
  if(!input.createdAt.empty()) out["created_at"]=input.createdAt;
  if(!input.metadataJson.empty()) out["_ip_nesting"]=Json::parse(input.metadataJson);
  out["timeLimitSeconds"]=input.config.timeLimitSeconds;
  out["timeLimitReached"]=run.bitmapStats.timeLimitReached;
  out["stopReason"]=run.bitmapStats.cancelled ? "user_stop" : run.bitmapStats.timeLimitReached ? "time_limit" : "completed";
  out["continuous"]=input.config.continuous;
  out["mode"]=input.config.mode==SearchMode::First ? "first" : input.config.mode==SearchMode::Timed ? "timed" : "continuous";
  out["continuousRoundSeconds"]=input.config.continuousRoundSeconds;
  out["searchIteration"]=run.searchIteration;
  const auto quality=layoutQuality(input.sheets,r,run.bitmapStats);
  out["usedSheetWasteArea"]=quality.usedSheetWasteArea;
  out["occupiedBoundsArea"]=run.bitmapStats.occupiedBoundsArea;
  out["compactWasteArea"]=std::max(0.0,run.bitmapStats.occupiedBoundsArea-r.area);
  out["startedTrials"]=run.bitmapStats.startedTrials;
  out["gpu"]={{"requested",input.config.gpuEnabled},{"backend","opencl"},
    {"used",run.bitmapStats.gpuBatches>0},{"device",run.bitmapStats.gpuDevice},
    {"candidates",run.bitmapStats.gpuCandidates},{"batches",run.bitmapStats.gpuBatches},
    {"fallbackReason",run.bitmapStats.gpuFallbackReason}};
  out["strategyResults"]=Json::array();
  for(const auto& t:run.bitmapStats.trials) out["strategyResults"].push_back({{"strategy",t.strategy},
    {"placed",t.placed},{"patternPlacements",t.patternPlacements},{"elapsedMs",t.elapsedMs},{"completed",t.completed},
    {"phasesMs",{{"preparation",t.phases.preparationMs},{"proposals",t.phases.proposalsMs},
      {"search",t.phases.searchMs},{"refinement",t.phases.refinementMs},{"gpuIncludingTransfers",t.phases.gpuMs}}}});
  std::unordered_map<int,const Polygon*> parts;
  for(const auto& p:input.individual.placement) if(p.id) parts.emplace(*p.id,&p);
  for(const auto& s:r.placements) {
    const auto sheet=std::find_if(input.sheets.begin(),input.sheets.end(),[&](const auto& p){return p.id==s.sheetid;});
    Json sheetResult={{"id",s.sheetid.value_or(0)},{"source",s.sheet},{"parts",Json::array()}};
    if(sheet!=input.sheets.end()) {
      sheetResult["points"]=pointsJson(*sheet); sheetResult["holes"]=holesJson(*sheet);
      if(!sheet->metadataJson.empty()) sheetResult["_ip_nesting"]=Json::parse(sheet->metadataJson);
    }
    for(const auto& p:s.sheetplacements) {
      const auto found=parts.find(p.id.value_or(0));
      if(found==parts.end()) throw std::runtime_error("Result references an unknown part instance");
      const auto absolute=shiftPolygon(rotatePolygon(*found->second,p.rotation),{p.x,p.y,true});
      sheetResult["parts"].push_back({{"id",p.id.value_or(0)},{"source",p.source},{"x",p.x},{"y",p.y},
        {"rotation",p.rotation},{"points",pointsJson(absolute)},{"holes",holesJson(absolute)}});
      if(!found->second->metadataJson.empty()) sheetResult["parts"].back()["_ip_nesting"]=Json::parse(found->second->metadataJson);
    }
    out["sheets"].push_back(std::move(sheetResult));
  }
  for(const auto& p:r.unplaced) {
    out["unplaced"].push_back({{"id",p.id.value_or(0)},{"source",p.source}});
    if(!p.metadataJson.empty()) out["unplaced"].back()["_ip_nesting"]=Json::parse(p.metadataJson);
  }
  std::ofstream stream(path,std::ios::binary);
  if(!stream) throw std::runtime_error("Cannot create result JSON: "+path.string());
  stream<<out.dump(2)<<'\n';
  if(!stream) throw std::runtime_error("Failed to write result JSON");
}
}
