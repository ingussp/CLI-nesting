#pragma once
#include <algorithm>
#include <vector>

namespace clinesting::detail {
struct PixelRect { int x{},y{},width{},height{}; };

// An overestimate of free bitmap space, subtracting only proven occupied
// rectangles. If a filled core cannot fit here, its entire mask cannot fit.
class FreeRectangles {
 public:
  FreeRectangles(int width,int height):free_{{0,0,width,height}} {}
  const std::vector<PixelRect>& regions() const { return free_; }
  bool fits(int width,int height) const {
    if(!enabled_ || width<=0 || height<=0) return true;
    return std::any_of(free_.begin(),free_.end(),[&](const auto& r) {
      return width<=r.width && height<=r.height;
    });
  }
  void occupy(PixelRect used) {
    if(!enabled_ || used.width<=0 || used.height<=0) return;
    std::vector<PixelRect> next;
    for(const auto& r:free_) {
      const int left=std::max(r.x,used.x),right=std::min(r.x+r.width,used.x+used.width);
      const int bottom=std::max(r.y,used.y),top=std::min(r.y+r.height,used.y+used.height);
      if(left>=right || bottom>=top) {next.push_back(r);continue;}
      if(left>r.x) next.push_back({r.x,r.y,left-r.x,r.height});
      if(right<r.x+r.width) next.push_back({right,r.y,r.x+r.width-right,r.height});
      if(bottom>r.y) next.push_back({r.x,r.y,r.width,bottom-r.y});
      if(top<r.y+r.height) next.push_back({r.x,top,r.width,r.y+r.height-top});
    }
    // Falling back to the original search is safe if fragmentation is large.
    if(next.size()>4096) {enabled_=false;free_.clear();return;}
    free_.clear();
    for(size_t i=0;i<next.size();++i) {
      const auto& a=next[i];bool contained=false;
      for(size_t j=0;j<next.size();++j) if(i!=j) {
        const auto& b=next[j];
        if(a.x>=b.x && a.y>=b.y && a.x+a.width<=b.x+b.width && a.y+a.height<=b.y+b.height &&
           (i>j || a.x!=b.x || a.y!=b.y || a.width!=b.width || a.height!=b.height)) {
          contained=true;break;
        }
      }
      if(!contained) free_.push_back(a);
    }
  }
 private:
  bool enabled_{true};
  std::vector<PixelRect> free_;
};
}
