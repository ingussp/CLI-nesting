// Included inside bitmap_nesting.cpp's anonymous namespace.
// Move already placed copies into open concavities, without touching live raster/GPU state.
void relocateIntoConcavePockets(const std::vector<Polygon>& sheets,const std::vector<Polygon>& parts,
    PlacementResult& result,const Config& config,const SearchDeadline& deadline,BitmapNestingStats& stats) {
  std::unordered_map<int,const Polygon*> originals;
  for(const auto& part:parts) if(part.id) originals.emplace(*part.id,&part);
  NfpCache cache;
  for(auto& layout:result.placements) {
    if(deadline.expired()) return;
    const auto sheet=std::find_if(sheets.begin(),sheets.end(),[&](const auto& p){return p.id==layout.sheetid;});
    if(sheet==sheets.end()) throw std::invalid_argument("Unknown sheet in pocket repair");
    const auto sb=getPolygonBounds(sheet->points);
    std::vector<Polygon> geometry;
    std::vector<Bounds> bounds;
    for(const auto& placed:layout.sheetplacements) {
      const auto source=originals.at(placed.id.value());
      geometry.push_back(shiftPolygon(rotatePolygon(*source,placed.rotation),{placed.x,placed.y,true}));
      bounds.push_back(getPolygonBounds(geometry.back().points));
    }
    std::vector<size_t> hosts(geometry.size()),donors(geometry.size());
    std::iota(hosts.begin(),hosts.end(),0);std::iota(donors.begin(),donors.end(),0);
    std::vector<double> cavityAreas;
    for(const auto& shape:geometry)
      cavityAreas.push_back(std::abs(polygonArea(getHull(shape.points)))-std::abs(polygonArea(shape)));
    std::stable_sort(hosts.begin(),hosts.end(),[&](size_t a,size_t b){return cavityAreas[a]>cavityAreas[b];});
    auto distance=[&](size_t i){const auto& b=bounds[i];return b.x+b.width/2+b.y+b.height/2;};
    std::stable_sort(donors.begin(),donors.end(),[&](size_t a,size_t b){return distance(a)>distance(b);});
    std::vector<bool> moved(geometry.size(),false);
    for(size_t host:hosts) {
      if(deadline.expired()) return;
      if(cavityAreas[host]<=1e-6) continue;
      // Hull minus outline exposes open notches; children remain real holes handled
      // by the existing hole placer. These regions are search hints, never material.
      auto hull=toClipperCoordinates(getHull(geometry[host].points),config.clipperScale);
      auto outer=outerPathToClipperCoordinates(geometry[host],config);
      if(!Clipper2Lib::IsPositive(hull)) std::reverse(hull.begin(),hull.end());
      if(!Clipper2Lib::IsPositive(outer)) std::reverse(outer.begin(),outer.end());
      auto pockets=Clipper2Lib::Difference({hull},{outer},Clipper2Lib::FillRule::NonZero);
      if(config.spacing>0) pockets=Clipper2Lib::InflatePaths(pockets,-config.spacing*config.clipperScale,
          Clipper2Lib::JoinType::Round,Clipper2Lib::EndType::Polygon,2.0,0.001*config.clipperScale);
      for(const auto& path:pockets) {
        Polygon pocket;pocket.points=toNestCoordinates(path,config.clipperScale);
        const double pocketArea=std::abs(polygonArea(pocket));
        const auto pb=getPolygonBounds(pocket.points);
        if(pocketArea<=1e-6) continue;
        for(size_t donor:donors) {
          if(deadline.expired()) return;
          if(donor==host || moved[donor]) continue;
          const auto& source=*originals.at(layout.sheetplacements[donor].id.value());
          if(polygonMaterialArea(source)>pocketArea+1e-6) continue;
          const double oldCost=placementSpreadCost(geometry[donor],sb);
          const auto& oldBounds=bounds[donor];
          if(oldBounds.x+oldBounds.width/2+oldBounds.y+oldBounds.height/2<=pb.x+pb.y) continue;
          std::vector<double> angles{layout.sheetplacements[donor].rotation};
          const size_t samples=std::min<size_t>(16,source.allowedAngles.size());
          for(size_t i=0;i<samples;++i) {
            const double angle=source.allowedAngles[(i*source.allowedAngles.size()/samples+config.searchIteration)%source.allowedAngles.size()];
            if(std::find(angles.begin(),angles.end(),angle)==angles.end()) angles.push_back(angle);
          }
          double bestCost=oldCost;
          std::optional<Placement> best;
          Polygon bestGeometry;
          for(double angle:angles) {
            if(deadline.expired()) break;
            const auto rotated=rotatePolygon(source,angle);
            const auto rb=getPolygonBounds(rotated.points);
            if(rb.width>pb.width+1e-8 || rb.height>pb.height+1e-8) continue;
            const auto feasible=getInnerNfp(pocket,rotated,config,cache);
            if(!feasible) continue;
            for(const auto& region:*feasible) for(const auto& point:region.points) {
              if(deadline.expired()) break;
              const double x=point.x-rotated.points.front().x,y=point.y-rotated.points.front().y;
              auto candidate=shiftPolygon(rotated,{x,y,true});
              const double cost=placementSpreadCost(candidate,sb);
              if(cost>=bestCost-std::max(1.0,std::abs(bestCost))*1e-9) continue;
              ++stats.vectorChecks;
              // Inner NFP vertices are proposals, not a proof for concave polygons.
              if(hasMaterialOutsideSheet(candidate,pocket,config) || violatesSheetClearance(candidate,*sheet,config)) continue;
              const auto cb=getPolygonBounds(candidate.points);
              const double gap=std::max(config.spacing,config.holeSpacing);
              const Bounds expanded{cb.x-gap,cb.y-gap,cb.width+2*gap,cb.height+2*gap};
              bool valid=true;
              for(size_t i=0;i<geometry.size();++i) if(i!=donor && boundsIntersect(expanded,bounds[i])) {
                ++stats.vectorChecks;
                if(violatesPartClearance(candidate,geometry[i],config)) {valid=false;break;}
              }
              if(!valid) continue;
              auto placement=layout.sheetplacements[donor];placement.x=x;placement.y=y;placement.rotation=angle;
              best=placement;bestGeometry=std::move(candidate);bestCost=cost;
            }
          }
          if(best) {
            layout.sheetplacements[donor]=*best;geometry[donor]=std::move(bestGeometry);
            bounds[donor]=getPolygonBounds(geometry[donor].points);moved[donor]=true;
            ++stats.pocketRelocations;++stats.vectorMoves;
          }
        }
      }
    }
  }
}
