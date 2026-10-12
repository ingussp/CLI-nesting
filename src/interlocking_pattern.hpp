// Included inside bitmap_nesting.cpp. Pitches are proposals from clearance
// NFPs; exact contour checks certify every potentially interacting cell pair.
bool betterPattern(const PairPattern& a,const PairPattern& b) {
  return a.capacity()>b.capacity() || (a.capacity()==b.capacity() &&
      int64_t(a.width)*a.height*b.members<int64_t(b.width)*b.height*a.members);
}

void improveInterlockingPattern(PairPattern& best,const std::vector<const RasterMask*>& masks,
    int sheetW,int sheetH,const Config& cfg,NfpCache& cache,const SearchDeadline& deadline) {
  if(!best.capacity()) return;
  auto motif=best.group;
  if(motif.empty()) {motif.push_back(best.a);if(best.members==2) motif.push_back(best.b);}
  const double resolution=cfg.bitmapResolutionMm;
  const int gap=int(std::ceil(std::max(cfg.spacing,cfg.holeSpacing)/resolution));
  const int edge=int(std::ceil(cfg.sheetSpacing/resolution));
  int left=INT_MAX,bottom=INT_MAX,right=INT_MIN,top=INT_MIN;
  for(const auto& c:motif) {
    left=std::min(left,c.x);bottom=std::min(bottom,c.y);
    right=std::max(right,c.x+masks[c.rotation]->widthPx);
    top=std::max(top,c.y+masks[c.rotation]->heightPx);
  }
  const int w=right-left,h=top-bottom;
  for(auto& c:motif) {c.x-=left;c.y-=bottom;}
  if(w>sheetW-2*edge || h>sheetH-2*edge) return;
  std::vector<Polygon> contours;
  std::vector<Bounds> bounds;
  for(const auto& c:motif) {
    const auto& m=*masks[c.rotation];
    contours.push_back(shiftPolygon(m.rotatedPart,{c.x*resolution-m.minX,c.y*resolution-m.minY,true}));
    bounds.push_back(getPolygonBounds(contours.back().points));
  }
  std::vector<std::vector<Point>> boundaries;
  for(const auto& a:motif) for(const auto& b:motif) {
    deadline.check();const auto& am=*masks[a.rotation];const auto& bm=*masks[b.rotation];
    const auto nfp=getOuterNfp(am.rotatedPart,bm.rotatedPart,false,cfg,cache);
    if(!nfp) continue;
    auto boundary=clearanceContacts(*nfp,cfg);
    for(auto& p:boundary) {
      p.x=(p.x-bm.rotatedPart.points.front().x-am.minX+bm.minX)/resolution+a.x-b.x;
      p.y=(p.y-bm.rotatedPart.points.front().y-am.minY+bm.minY)/resolution+a.y-b.y;
    }
    boundaries.push_back(std::move(boundary));
  }
  std::unordered_map<uint64_t,bool> compatibility;
  auto compatible=[&](int x,int y) {
    deadline.check();
    if(y<0 || (y==0 && x<0)) {x=-x;y=-y;}
    const auto key=(uint64_t(uint32_t(x))<<32)|uint32_t(y);
    if(const auto it=compatibility.find(key);it!=compatibility.end()) return it->second;
    const double dx=x*resolution,dy=y*resolution;
    const double clearance=std::max(cfg.spacing,cfg.holeSpacing)+1e-7;
    bool valid=true;
    for(size_t b=0;b<contours.size() && valid;++b) {
      const auto moved=shiftPolygon(contours[b],{dx,dy,true});
      for(size_t a=0;a<contours.size();++a) {
        const auto& ab=bounds[a];const auto& bb=bounds[b];
        if(ab.x>bb.x+dx+bb.width+clearance || bb.x+dx>ab.x+ab.width+clearance ||
           ab.y>bb.y+dy+bb.height+clearance || bb.y+dy>ab.y+ab.height+clearance) continue;
        if(violatesPartClearance(contours[a],moved,cfg)) {valid=false;break;}
      }
    }
    compatibility.emplace(key,valid);return valid;
  };
  auto addRounded=[](std::vector<int>& values,double value,int low,int high) {
    if(!std::isfinite(value) || value<low-1.0 || value>high+1.0) return;
    const int v=int(std::floor(value));
    for(int d:{0,1,2}) if(v+d>=low && v+d<=high) values.push_back(v+d);
  };
  auto unique=[](std::vector<int>& values) {
    std::sort(values.begin(),values.end());values.erase(std::unique(values.begin(),values.end()),values.end());
  };
  // Limiting compression to a quarter footprint bounds preparation work. It is
  // a heuristic, never a claim to enumerate every possible packing.
  const int minX=std::max(1,(w+gap+3)/4),minY=std::max(1,(h+gap+3)/4);
  std::vector<int> horizontal{w+gap};
  for(const auto& path:boundaries) for(size_t i=0;i<path.size();++i) {
    const auto& a=path[i];const auto& b=path[(i+1)%path.size()];
    if((a.y<=0 && b.y>=0)||(b.y<=0 && a.y>=0)) {
      if(std::abs(a.y-b.y)>1e-10) addRounded(horizontal,a.x+(b.x-a.x)*(-a.y)/(b.y-a.y),minX,w+gap);
      else {addRounded(horizontal,a.x,minX,w+gap);addRounded(horizontal,b.x,minX,w+gap);}
    }
  }
  unique(horizontal);
  std::vector<int> pitches;
  for(int x:horizontal) {
    bool valid=true;
    for(int k=1;k*x<=w+gap;++k) if(!compatible(k*x,0)) {valid=false;break;}
    if(valid) {pitches.push_back(x);if(pitches.size()==4) break;}
  }
  if(std::find(pitches.begin(),pitches.end(),w+gap)==pitches.end()) pitches.push_back(w+gap);
  for(int x:pitches) {
    std::vector<int> shifts{0,x/4,x/2,3*x/4};unique(shifts);
    for(int shift:shifts) {
      deadline.check();
      std::vector<int> vertical{h+gap};
      // Intersections at neighbouring column offsets supply row contact
      // heights, including second/further rows of the alternating pattern.
      for(int row=1;row<=4;++row) for(int sign:{-1,1}) {
        deadline.check();
        const int offset=row%2?sign*shift:0;
        const int extent=(w+gap+std::abs(offset))/x+1;
        for(int k=-extent;k<=extent;++k) {
          const double line=offset+k*x;
          for(const auto& path:boundaries) for(size_t i=0;i<path.size();++i) {
            const auto& a=path[i];const auto& b=path[(i+1)%path.size()];
            if((a.x<=line && b.x>=line)||(b.x<=line && a.x>=line)) {
              if(std::abs(a.x-b.x)>1e-10)
                addRounded(vertical,(a.y+(b.y-a.y)*(line-a.x)/(b.x-a.x))/row,minY,h+gap);
              else {addRounded(vertical,a.y/row,minY,h+gap);addRounded(vertical,b.y/row,minY,h+gap);}
            }
          }
        }
      }
      unique(vertical);
      for(int y:vertical) {
        PairPattern candidate=best;
        candidate.width=x;candidate.height=y;candidate.rowShift=shift;
        candidate.columns=1+(sheetW-2*edge-w)/x;
        candidate.oddColumns=sheetW-2*edge-w>=shift ? 1+(sheetW-2*edge-w-shift)/x : 0;
        candidate.rows=1+(sheetH-2*edge-h)/y;
        if(!candidate.oddColumns) candidate.rows=1;
        if(!betterPattern(candidate,best)) continue;
        bool valid=true;
        for(int row=1;row<candidate.rows && row*y<=h+gap && valid;++row) {
          for(int sign:{-1,1}) {
            // For two rows only the even-to-odd direction exists.
            if(sign<0 && row+1>=candidate.rows) continue;
            const int offset=row%2?sign*shift:0;
            const int extent=(w+gap+std::abs(offset))/x+1;
            for(int k=-extent;k<=extent;++k) if(!compatible(offset+k*x,row*y)) {valid=false;break;}
            if(!valid) break;
          }
        }
        if(valid) {
          candidate.interlocking=x<w+gap || y<h+gap;
          candidate.group=motif;
          for(auto& c:candidate.group) {c.x+=edge;c.y+=edge;}
          best=std::move(candidate);
        }
      }
    }
  }
}
