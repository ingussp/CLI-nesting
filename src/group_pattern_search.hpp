// Included in bitmap_nesting.cpp after clearanceContacts and PairPattern.
void improveGroupPattern(PairPattern& best,const PairPattern& seed,const std::vector<const RasterMask*>& masks,
    int sheetW,int sheetH,const Config& cfg,NfpCache& cache,const SearchDeadline& deadline) {
  if(!best.capacity()) return;
  struct Group {std::vector<Candidate> members;int width=0,height=0;uint64_t capacity=0;};
  const int gap=int(std::ceil(std::max(cfg.spacing,cfg.holeSpacing)/cfg.bitmapResolutionMm));
  const int edge=int(std::ceil(cfg.sheetSpacing/cfg.bitmapResolutionMm));
  auto measure=[&](Group& g) {
    int left=INT_MAX,bottom=INT_MAX,right=INT_MIN,top=INT_MIN;
    for(const auto& c:g.members) {left=std::min(left,c.x);bottom=std::min(bottom,c.y);right=std::max(right,c.x+masks[c.rotation]->widthPx);top=std::max(top,c.y+masks[c.rotation]->heightPx);}
    for(auto& c:g.members) {c.x-=left;c.y-=bottom;}
    g.width=right-left+gap;g.height=top-bottom+gap;
    g.capacity=uint64_t(std::max(0,sheetW-2*edge+gap)/g.width)*uint64_t(std::max(0,sheetH-2*edge+gap)/g.height)*g.members.size();
  };
  auto rank=[](const Group& a,const Group& b) {
    if(a.capacity!=b.capacity) return a.capacity>b.capacity;
    return std::tuple{int64_t(a.width)*a.height,a.height,a.width}<std::tuple{int64_t(b.width)*b.height,b.height,b.width};
  };
  Group pair;pair.members={seed.a};if(seed.members==2) pair.members.push_back(seed.b);measure(pair);
  std::vector<Group> beam{pair};
  // A bounded beam branches through pair-contact positions up to eight copies.
  for(size_t depth=pair.members.size();depth<8;++depth) {
    std::vector<Group> next;
    for(const auto& parent:beam) {
      deadline.check();std::vector<Group> proposals;
      for(size_t r=0;r<masks.size();r+=std::max<size_t>(1,(masks.size()+11)/12)) {
        deadline.check();const auto& moving=*masks[r];
        for(const auto& anchor:parent.members) {
          const auto& fixed=*masks[anchor.rotation];
          const auto nfp=getOuterNfp(fixed.rotatedPart,moving.rotatedPart,false,cfg,cache);
          if(!nfp) continue;
          const auto contacts=clearanceContacts(*nfp,cfg);
          for(size_t j=0;j<contacts.size();j+=std::max<size_t>(1,(contacts.size()+31)/32)) {
            const auto& p=contacts[j];
            const int x=int(std::floor(anchor.x+(p.x-moving.rotatedPart.points.front().x-fixed.minX+moving.minX)/cfg.bitmapResolutionMm));
            const int y=int(std::floor(anchor.y+(p.y-moving.rotatedPart.points.front().y-fixed.minY+moving.minY)/cfg.bitmapResolutionMm));
            for(int dx:{0,1}) for(int dy:{0,1}) {
              Group g=parent;g.members.push_back({x+dx,y+dy,r});measure(g);
              if(g.capacity) proposals.push_back(std::move(g));
            }
          }
        }
      }
      const size_t limit=std::min<size_t>(512,proposals.size());
      if(limit<proposals.size()) std::nth_element(proposals.begin(),proposals.begin()+limit,proposals.end(),rank);
      std::sort(proposals.begin(),proposals.begin()+limit,rank);
      size_t accepted=0;
      for(size_t i=0;i<limit && accepted<4;++i) {
        deadline.check();auto& g=proposals[i];const auto& c=g.members.back();const auto& m=*masks[c.rotation];
        const auto geometry=shiftPolygon(m.rotatedPart,{c.x*cfg.bitmapResolutionMm-m.minX,c.y*cfg.bitmapResolutionMm-m.minY,true});
        bool valid=true;
        for(size_t j=0;j+1<g.members.size();++j) {
          const auto& q=g.members[j];const auto& qm=*masks[q.rotation];
          const auto other=shiftPolygon(qm.rotatedPart,{q.x*cfg.bitmapResolutionMm-qm.minX,q.y*cfg.bitmapResolutionMm-qm.minY,true});
          if(violatesPartClearance(geometry,other,cfg)) {valid=false;break;}
        }
        if(valid) {next.push_back(std::move(g));++accepted;}
      }
    }
    if(next.empty()) break;
    best.searchedMembers=std::max(best.searchedMembers,depth+1);
    std::sort(next.begin(),next.end(),rank);if(next.size()>4) next.resize(4);beam=std::move(next);
    for(size_t i=0;i<beam.size();++i) {
      const auto& g=beam[i];PairPattern candidate;
      candidate.width=g.width;candidate.height=g.height;candidate.members=int(g.members.size());candidate.group=g.members;
      for(auto& c:candidate.group) {c.x+=edge;c.y+=edge;}
      candidate.columns=std::max(0,sheetW-2*edge+gap)/g.width;candidate.rows=std::max(0,sheetH-2*edge+gap)/g.height;
      candidate.searchedMembers=best.searchedMembers;
      if(betterPattern(candidate,best)) best=candidate;
      // A group's disjoint bounding-box score must not exclude it from the
      // contour-fitting search. Bound the extra work to the leading beam item.
      if(i==0) {
        improveInterlockingPattern(candidate,masks,sheetW,sheetH,cfg,cache,deadline);
        if(betterPattern(candidate,best)) best=std::move(candidate);
      }
    }
  }
}
