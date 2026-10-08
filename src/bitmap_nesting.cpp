#include "clinesting/bitmap_nesting.hpp"
#include "clinesting/continuous_nesting.hpp"

#include "clinesting/geometry.hpp"
#include "clinesting/nfp.hpp"
#include "clinesting/gpu_bitmap.hpp"
#include "parallel_loop.hpp"
#include "search_deadline.hpp"
#include "raster_scanline.hpp"
#include "free_rectangles.hpp"
#include "clearance_index.hpp"
#include "convex_pair_cache.hpp"
#include "rejection_cache.hpp"
#include <random>

#include <algorithm>
#include <bit>
#include <atomic>
#include <thread>
#include <mutex>
#include <exception>
#include <chrono>
#include <clipper2/clipper.h>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>
#include <sstream>
#include <iomanip>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__AVX2__) || (defined(__GNUC__) && !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__)))
#include <immintrin.h>
#endif

namespace clinesting {

namespace {

constexpr size_t kMaxBitmapPixels = 200000000ULL;

// Accumulate elapsed time into a search-phase counter on scope exit.
struct PhaseTimer {
  double& total;
  std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
  // Add this scope's elapsed duration to its phase total.
  ~PhaseTimer() { total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); }
};

// Convert a physical dimension to a bounded positive pixel count.
int rasterDimension(double mm, double resolution) {
  const double pixels=std::ceil(mm/resolution);
  if (!std::isfinite(pixels) || pixels>double(kMaxBitmapPixels))
    throw std::invalid_argument("Raster is too large; increase --bitmap-resolution");
  return std::max(1,static_cast<int>(pixels));
}

// Store a row-packed bitmap of stock or occupied material.
struct BitmapGrid {
  int widthPx{0};
  int heightPx{0};
  size_t wordsPerRow{0};
  std::vector<uint64_t> bits;
};

// Store one rotated contour, contact points and lazy raster pixels.
struct RasterMask {
  Polygon rotatedPart;
  double rotationDeg{0.0};
  Bounds rotatedBounds;
  double minX{0.0};
  double minY{0.0};
  int widthPx{0};
  int heightPx{0};
  size_t wordsPerRow{0};
  mutable std::vector<uint64_t> bits;
  std::vector<Point> contacts;
  mutable std::vector<int> collisionRows;
  mutable detail::PixelRect filledCore;
  mutable detail::PixelRect geometricCore;
  mutable bool coreVerified{false};
};

// Uniform spatial bins only select neighbours. Original polygons remain authoritative.
class SpatialIndex {
 public:
  // Initialize uniform bins for nearby polygon lookup.
  explicit SpatialIndex(double cellSize) : cellSize_(cellSize) {}
  // Add an entry to the current spatial or rejected-origin index.
  void insert(const Bounds& b, size_t id) {
    visit(b, [&](uint64_t key) { cells_[key].push_back(id); });
  }
  // Collect unique nearby polygon indices intersecting a query rectangle.
  std::vector<size_t> query(const Bounds& b) const {
    std::vector<size_t> ids;
    visit(b, [&](uint64_t key) {
      auto it = cells_.find(key);
      if (it != cells_.end()) ids.insert(ids.end(), it->second.begin(), it->second.end());
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
  }
 private:
  // Visit the spatial cells touched by a bounding rectangle.
  template<class F> void visit(const Bounds& b, F f) const {
    const int x0 = static_cast<int>(std::floor(b.x / cellSize_));
    const int y0 = static_cast<int>(std::floor(b.y / cellSize_));
    const int x1 = static_cast<int>(std::floor((b.x + b.width) / cellSize_));
    const int y1 = static_cast<int>(std::floor((b.y + b.height) / cellSize_));
    for (int x = x0; x <= x1; ++x)
      for (int y = y0; y <= y1; ++y)
        f((uint64_t(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(y));
  }
  double cellSize_;
  std::unordered_map<uint64_t, std::vector<size_t>> cells_;
};

// Refine only after bitmap placement has finished: raster occupancy is never
// reused after these continuous-coordinate moves. Every committed move is exact-validated.
void refineVectors(const std::vector<Polygon>& sheets,const std::vector<Polygon>& parts,
                   PlacementResult& result,const Config& cfg,const SearchDeadline& deadline,
                   BitmapNestingStats& stats) {
  const auto started=std::chrono::steady_clock::now();
  constexpr double tolerance=0.001;
  std::unordered_map<int,const Polygon*> byId;
  for(const auto& p:parts) if(p.id) byId.emplace(*p.id,&p);
  bool stopped=false;
  double occupied=0;
  for(auto& layout:result.placements) {
    const auto sheet=std::find_if(sheets.begin(),sheets.end(),[&](const Polygon& p){return p.id==layout.sheetid;});
    if(sheet==sheets.end()) throw std::runtime_error("Unknown sheet in vector refinement");
    const auto sb=getPolygonBounds(sheet->points);
    SpatialIndex index(std::max(16.0,std::max(sb.width,sb.height)/32.0));
    std::vector<Polygon> absolute;
    detail::ConvexPairCache pairCache;
    std::vector<size_t> pairShapes;
    double coordinateScale=std::max({1.0,std::abs(sb.x),std::abs(sb.y),sb.width,sb.height});
    for(const auto& placement:layout.sheetplacements) {
      auto it=byId.find(placement.id.value_or(-1));
      if(it==byId.end()) throw std::runtime_error("Unknown part in vector refinement");
      const auto rotated=rotatePolygon(*it->second,placement.rotation);
      pairShapes.push_back(pairCache.add(rotated));
      absolute.push_back(shiftPolygon(rotated,{placement.x,placement.y,true}));
      for(const auto& pt:rotated.points) coordinateScale=std::max({coordinateScale,std::abs(pt.x),std::abs(pt.y)});
      coordinateScale=std::max({coordinateScale,std::abs(placement.x),std::abs(placement.y)});
      index.insert(getPolygonBounds(absolute.back().points),absolute.size()-1);
    }
    // Conservative band covers Clipper quantization and translation rounding.
    // Within the band, retain the unchanged exact predicate.
    const double pairGuard=std::max({1e-5,4.0/cfg.clipperScale,
        coordinateScale*128*std::numeric_limits<double>::epsilon()});
    const detail::ClearanceIndex sheetEdges(*sheet);
    const detail::RectangularSheetClearance sheetRectangle(*sheet);
    std::vector<detail::ClearanceIndex> partEdges;
    partEdges.reserve(absolute.size());
    for(const auto& p:absolute) partEdges.emplace_back(p);
    for(int pass=0;pass<3 && !stopped;++pass) {
      bool moved=false;
      for(size_t i=0;i<absolute.size() && !stopped;++i) for(int axis:{1,0}) {
        if(deadline.expired()) {stopped=true;break;}
        auto& placement=layout.sheetplacements[i];
        const auto bounds=getPolygonBounds(absolute[i].points);
        const double limit=std::max(0.0,axis ? bounds.y-sb.y-cfg.sheetSpacing : bounds.x-sb.x-cfg.sheetSpacing);
        if(limit<=tolerance) continue;
        auto validShift=[&](double amount) {
          if(deadline.expired()) {stopped=true;return false;}
          ++stats.vectorChecks;
          const Point shift{axis?0:-amount,axis?-amount:0,true};
          auto candidate=shiftPolygon(absolute[i],shift);
          auto b=getPolygonBounds(candidate.points);
          const auto stockCheck=sheetRectangle.violation(b,cfg.sheetSpacing,pairGuard);
          if(stockCheck) {if(*stockCheck) return false;}
          else if(partEdges[i].sheetViolation(candidate,*sheet,sheetEdges,shift,cfg)) return false;
          const double gap=std::max(cfg.spacing,cfg.holeSpacing);
          b.x-=gap;b.y-=gap;b.width+=2*gap;b.height+=2*gap;
          for(auto j:index.query(b)) if(j!=i) {
            if(deadline.expired()) {stopped=true;return false;}
            const auto& other=layout.sheetplacements[j];
            const Point relative{other.x-(placement.x+shift.x),other.y-(placement.y+shift.y),true};
            const auto quick=pairCache.violation(pairShapes[i],pairShapes[j],relative,cfg.spacing,pairGuard);
            if(quick) {if(*quick) return false;}
            else if(partEdges[i].partViolation(candidate,absolute[j],partEdges[j],shift,cfg)) return false;
          }
          return true;
        };
        double low=0,high=std::min(limit,cfg.bitmapResolutionMm);
        while(!stopped && validShift(high)) {
          low=high;
          if(high>=limit) break;
          high=std::min(limit,high*2);
        }
        while(!stopped && high-low>tolerance) {
          const double mid=(low+high)/2;
          if(validShift(mid)) low=mid;else high=mid;
        }
        if(low>tolerance) {
          absolute[i]=shiftPolygon(absolute[i],{axis?0:-low,axis?-low:0,true});
          partEdges[i]=detail::ClearanceIndex(absolute[i]);
          if(axis) placement.y-=low;else placement.x-=low;
          // Stale bin entries are conservative; query deduplication and exact
          // geometry validation ensure they cannot cause a missed neighbour.
          index.insert(getPolygonBounds(absolute[i].points),i);
          ++stats.vectorMoves;moved=true;
        }
      }
      if(!moved) break;
    }
    double right=0,top=0;
    for(const auto& p:absolute) {auto b=getPolygonBounds(p.points);right=std::max(right,b.x+b.width-sb.x);top=std::max(top,b.y+b.height-sb.y);}
    occupied+=right*top;
  }
  stats.occupiedBoundsArea=occupied;
  stats.vectorRefinementCompleted=!stopped;
  stats.vectorRefinementMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
}

// Record an accepted mask and origin for reusable proposals.
struct RasterPlacement {
  int x, y;
  const RasterMask* mask;
  std::string identity;
};

// Store one candidate origin, rotation and ranking information.
struct Candidate {
  int x, y;
  size_t rotation;
  int64_t area{0};
  int extent{0};
  const Polygon* pocket{nullptr};
};

using detail::RejectedOrigins;

// Check whether two axis-aligned bounds overlap with positive extent.
bool boundsIntersect(const Bounds& a, const Bounds& b) {
  const double aRight = a.x + a.width;
  const double aBottom = a.y + a.height;
  const double bRight = b.x + b.width;
  const double bBottom = b.y + b.height;
  return !(aRight <= b.x || bRight <= a.x || aBottom <= b.y || bBottom <= a.y);
}

// Classify a point using ray crossings of a simple contour.
bool pointInSimplePolygon(const std::vector<Point>& polygon, const Point& p) {
  if (polygon.size() < 3) {
    return false;
  }
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const auto& a = polygon[i];
    const auto& b = polygon[j];
    const bool intersects = ((a.y > p.y) != (b.y > p.y)) &&
                            (p.x < (b.x - a.x) * (p.y - a.y) / ((b.y - a.y) == 0.0 ? 1e-12 : (b.y - a.y)) + a.x);
    if (intersects) {
      inside = !inside;
    }
  }
  return inside;
}

// Check whether a point lies in the outline outside all holes.
bool pointInPolygonMaterial(const Polygon& polygon, const Point& p) {
  if (!pointInSimplePolygon(polygon.points, p)) {
    return false;
  }
  for (const auto& hole : polygon.children) {
    if (pointInSimplePolygon(hole.points, p)) {
      return false;
    }
  }
  return true;
}

// Reject raster dimensions that exceed the allocation budget.
void ensureBitmapFitsMemory(int widthPx, int heightPx, const char* what) {
  if (widthPx <= 0 || heightPx <= 0) {
    throw std::invalid_argument(std::string(what) + " raster dimensions must be positive");
  }

  const size_t pixels = static_cast<size_t>(widthPx) * static_cast<size_t>(heightPx);
  if (pixels > kMaxBitmapPixels) {
    throw std::invalid_argument(std::string(what) + " raster is too large; increase --bitmap-resolution");
  }
}

// Allocate an empty row-packed bitmap with validated dimensions.
BitmapGrid makeBitmapGrid(int widthPx, int heightPx) {
  ensureBitmapFitsMemory(widthPx, heightPx, "Bitmap");
  BitmapGrid grid;
  grid.widthPx = widthPx;
  grid.heightPx = heightPx;
  grid.wordsPerRow = static_cast<size_t>((widthPx + 63) / 64);
  grid.bits.assign(grid.wordsPerRow * static_cast<size_t>(heightPx), 0ULL);
  return grid;
}

// Access the packed words belonging to a bitmap row.
inline uint64_t* rowBits(BitmapGrid& grid, int y) {
  return &grid.bits[static_cast<size_t>(y) * grid.wordsPerRow];
}

// Access the packed words belonging to a bitmap row.
inline const uint64_t* rowBits(const BitmapGrid& grid, int y) {
  return &grid.bits[static_cast<size_t>(y) * grid.wordsPerRow];
}

// Set one material pixel in a packed bitmap.
void setPixel(BitmapGrid& grid, int x, int y) {
  if (x < 0 || y < 0 || x >= grid.widthPx || y >= grid.heightPx) {
    return;
  }
  const size_t idx = static_cast<size_t>(y) * grid.wordsPerRow + static_cast<size_t>(x / 64);
  grid.bits[idx] |= (1ULL << static_cast<unsigned>(x % 64));
}

// Detect runtime AVX2 support before choosing SIMD operations.
bool avx2RuntimeAvailable() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER)
  int regs[4] = {0, 0, 0, 0};
  __cpuidex(regs, 1, 0);
  const bool osxsave = (regs[2] & (1 << 27)) != 0;
  const bool avx = (regs[2] & (1 << 28)) != 0;
  if (!osxsave || !avx) {
    return false;
  }
  const unsigned long long xcr0 = _xgetbv(0);
  if ((xcr0 & 0x6) != 0x6) {
    return false;
  }
  __cpuidex(regs, 7, 0);
  return (regs[1] & (1 << 5)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
#else
  return false;
#endif
}

#if defined(__AVX2__)
// Check aligned row collisions and containment with AVX2.
bool rowsCollideAndInBoundsAvx2(const uint64_t* occ, const uint64_t* material, const uint64_t* mask, size_t words) {
  size_t i = 0;
  const size_t vecWords = (words / 4) * 4;
  for (; i < vecWords; i += 4) {
    const __m256i occVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(occ + i));
    const __m256i matVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(material + i));
    const __m256i maskVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mask + i));
    const __m256i overlap = _mm256_and_si256(maskVec, occVec);
    const __m256i outside = _mm256_andnot_si256(matVec, maskVec);
    if (!_mm256_testz_si256(overlap, overlap) || !_mm256_testz_si256(outside, outside)) {
      return true;
    }
  }
  for (; i < words; ++i) {
    const uint64_t overlap = occ[i] & mask[i];
    const uint64_t outside = (~material[i]) & mask[i];
    if ((overlap | outside) != 0ULL) {
      return true;
    }
  }
  return false;
}

// Merge aligned mask words into occupancy using AVX2.
void rowsOrAvx2(uint64_t* occ, const uint64_t* mask, size_t words) {
  size_t i = 0;
  const size_t vecWords = (words / 4) * 4;
  for (; i < vecWords; i += 4) {
    const __m256i occVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(occ + i));
    const __m256i maskVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(mask + i));
    const __m256i out = _mm256_or_si256(occVec, maskVec);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(occ + i), out);
  }
  for (; i < words; ++i) {
    occ[i] |= mask[i];
  }
}
#endif

// Check aligned row collisions and containment with scalar operations.
bool rowsCollideAndInBoundsScalar(const uint64_t* occ, const uint64_t* material, const uint64_t* mask, size_t words) {
  for (size_t i = 0; i < words; ++i) {
    const uint64_t overlap = occ[i] & mask[i];
    const uint64_t outside = (~material[i]) & mask[i];
    if ((overlap | outside) != 0ULL) {
      return true;
    }
  }
  return false;
}

// Merge aligned mask words into occupancy without SIMD.
void rowsOrScalar(uint64_t* occ, const uint64_t* mask, size_t words) {
  for (size_t i = 0; i < words; ++i) {
    occ[i] |= mask[i];
  }
}

// Prepare a rotated part's dimensions and simplified contact points.
RasterMask rasterizePartMask(const Polygon& part, double rotationDeg, double resolutionMm, double tolerance) {
  RasterMask mask;
  mask.rotationDeg = rotationDeg;
  mask.rotatedPart = rotatePolygon(part, rotationDeg);
  mask.rotatedPart.rotation = rotationDeg;
  mask.rotatedPart.id = part.id;
  mask.rotatedPart.source = part.source;
  mask.rotatedPart.filename = part.filename;

  const Bounds bounds = getPolygonBounds(mask.rotatedPart.points);
  mask.rotatedBounds = bounds;
  mask.minX = bounds.x;
  mask.minY = bounds.y;
  // Simplification affects contact proposals only, never occupancy, validation or DXF.
  Clipper2Lib::PathD contour;
  for (const auto& p : mask.rotatedPart.points) contour.emplace_back(p.x, p.y);
  if (tolerance > 0.0) contour = Clipper2Lib::SimplifyPath(contour, tolerance);
  const size_t stride = std::max<size_t>(1, (contour.size() + 15) / 16);
  for (size_t i = 0; i < contour.size(); i += stride)
    mask.contacts.push_back({(contour[i].x - bounds.x) / resolutionMm,
                             (contour[i].y - bounds.y) / resolutionMm, true});
  mask.widthPx = rasterDimension(bounds.width,resolutionMm);
  mask.heightPx = rasterDimension(bounds.height,resolutionMm);
  ensureBitmapFitsMemory(mask.widthPx, mask.heightPx, "Part");

  mask.wordsPerRow = static_cast<size_t>((mask.widthPx + 63) / 64);
  return mask;
}

// Keep the exact rotated geometry for every angle, but build pixel data only
// when a candidate actually needs collision testing.
void rasterizeMaskPixels(const RasterMask& mask, double resolutionMm, const SearchDeadline* deadline, ParallelLoop& parallel) {
  mask.bits.assign(mask.wordsPerRow * static_cast<size_t>(mask.heightPx), 0ULL);

  parallel.run(size_t(mask.heightPx),[&](size_t begin,size_t end,size_t) {
  std::vector<double> crossings;
  for (int y = int(begin); y < int(end); ++y) {
    if(deadline) deadline->check();
    const double py = mask.minY + (static_cast<double>(y) + 0.5) * resolutionMm;
    auto fill=[&](const Polygon& contour,bool material) {
      detail::rasterContourRow(contour.points,py,mask.minX,resolutionMm,mask.widthPx,crossings,[&](int first,int last) {
        while(first<last) {
          const int word=first/64,offset=first%64,n=std::min(last-first,64-offset);
          const uint64_t bits=(~uint64_t(0)>>(64-n))<<offset;
          auto& value=mask.bits[size_t(y)*mask.wordsPerRow+size_t(word)];
          if(material) value|=bits; else value&=~bits;
          first+=n;
        }
      });
    };
    fill(mask.rotatedPart,true);
    for(const auto& hole:mask.rotatedPart.children) fill(hole,false);
  }

  });

  // Dense rows reject collisions early; empty rows cannot collide or be outside
  // the material and can be omitted. Storage/commit order remains unchanged.
  std::vector<std::pair<int,int>> density;
  for (int y = 0; y < mask.heightPx; ++y) {
    if(deadline) deadline->check();
    int count = 0;
    for (size_t w = 0; w < mask.wordsPerRow; ++w)
      count += std::popcount(mask.bits[size_t(y) * mask.wordsPerRow + w]);
    if (count) density.emplace_back(-count, y);
  }
  std::sort(density.begin(), density.end());
  mask.collisionRows.reserve(density.size());
  for (const auto& entry : density) mask.collisionRows.push_back(entry.second);
  mask.filledCore={};
  if(!density.empty()) {
    int ymin=mask.heightPx,ymax=0;
    for(const auto& entry:density) {ymin=std::min(ymin,entry.second);ymax=std::max(ymax,entry.second);}
    std::vector<uint64_t> common(mask.wordsPerRow,~uint64_t(0));
    for(int y=ymin;y<=ymax;++y) {
      if(deadline) deadline->check();
      for(size_t w=0;w<common.size();++w) common[w]&=mask.bits[size_t(y)*mask.wordsPerRow+w];
    }
    int start=0;
    for(int x=0;x<=mask.widthPx;++x) {
      if(x<mask.widthPx && (common[size_t(x)/64]&(uint64_t(1)<<(x%64)))) continue;
      if(x-start>mask.filledCore.width) mask.filledCore={start,ymin,x-start,ymax-ymin+1};
      start=x+1;
    }
    if(int64_t(mask.filledCore.width)*mask.filledCore.height < int64_t(mask.widthPx)*(ymax-ymin+1)*9/10) {
      // Edge notches can make a full-height stripe very narrow. A histogram
      // finds the largest genuinely filled rectangle without approximating
      // holes, concavities, or a curved boundary as solid material.
      std::vector<int> heights(size_t(mask.widthPx),0),stack;
      stack.reserve(size_t(mask.widthPx)+1);
      for(int y=ymin;y<=ymax;++y) {
        if(deadline) deadline->check();
        stack.clear();
        for(int x=0;x<=mask.widthPx;++x) {
          const int h=x==mask.widthPx ? 0 :
            (mask.bits[size_t(y)*mask.wordsPerRow+size_t(x)/64]&(uint64_t(1)<<(x%64))) ? heights[size_t(x)]+1 : 0;
          if(x<mask.widthPx) heights[size_t(x)]=h;
          while(!stack.empty() && heights[size_t(stack.back())]>h) {
            const int height=heights[size_t(stack.back())];stack.pop_back();
            const int left=stack.empty() ? 0 : stack.back()+1;
            if(int64_t(x-left)*height>int64_t(mask.filledCore.width)*mask.filledCore.height)
              mask.filledCore={left,y-height+1,x-left,height};
          }
          stack.push_back(x);
        }
      }
    }
  }
}

// Pixel data can be discarded without invalidating placements, which retain
// stable pointers to the mask's geometry. Rebuild it on demand after eviction.
class RasterPixels {
 public:
  // Initialize the bounded lazy raster cache.
  explicit RasterPixels(double resolution,const SearchDeadline& deadline,ParallelLoop& parallel) : resolution_(resolution),deadline_(deadline),parallel_(parallel) {}
  // Materialize a mask while respecting cache and cancellation limits.
  void ensure(const RasterMask& mask,bool cancellable=true,
              std::span<const RasterMask* const> pinned={}) {
    if(cancellable) deadline_.check();
    if (!mask.bits.empty()) return;
    const size_t estimate = mask.wordsPerRow * size_t(mask.heightPx) * sizeof(uint64_t)
                          + size_t(mask.heightPx) * sizeof(int);
    constexpr size_t budget = 64ULL * 1024 * 1024;
    while (!resident_.empty() && (estimate > budget || bytes_ > budget - estimate)) {
      const auto victim=std::find_if(resident_.begin(),resident_.end(),[&](const RasterMask* p) {
        return std::find(pinned.begin(),pinned.end(),p)==pinned.end();
      });
      if(victim==resident_.end()) break;
      const auto* old = *victim;
      resident_.erase(victim);
      bytes_ -= old->bits.capacity() * sizeof(uint64_t) + old->collisionRows.capacity() * sizeof(int);
      std::vector<uint64_t>().swap(old->bits);
      std::vector<int>().swap(old->collisionRows);
    }
    try { rasterizeMaskPixels(mask, resolution_,cancellable ? &deadline_ : nullptr,parallel_); }
    catch(const SearchTimeExpired&) { mask.bits.clear(); mask.collisionRows.clear(); throw; }
    bytes_ += mask.bits.capacity() * sizeof(uint64_t) + mask.collisionRows.capacity() * sizeof(int);
    resident_.push_back(&mask);
  }
 private:
  double resolution_;
  const SearchDeadline& deadline_;
  ParallelLoop& parallel_;
  size_t bytes_{0};
  std::deque<const RasterMask*> resident_;
};

// Convert usable stock, excluding holes, into a material bitmap.
BitmapGrid rasterizeSheetMaterial(const Polygon& sheet, double resolutionMm, Bounds& sheetBounds,const SearchDeadline& deadline,ParallelLoop& parallel) {
  sheetBounds = getPolygonBounds(sheet.points);
  const int widthPx = rasterDimension(sheetBounds.width,resolutionMm);
  const int heightPx = rasterDimension(sheetBounds.height,resolutionMm);
  BitmapGrid material = makeBitmapGrid(widthPx, heightPx);

  parallel.run(size_t(material.heightPx),[&](size_t begin,size_t end,size_t) {
  std::vector<double> crossings;
  for (int y = int(begin); y < int(end); ++y) {
    deadline.check();
    const double py=sheetBounds.y+(double(y)+0.5)*resolutionMm;
    auto draw=[&](const Polygon& contour,bool fill) {
      detail::rasterContourRow(contour.points,py,sheetBounds.x,resolutionMm,widthPx,crossings,[&](int begin,int end) {
        auto* row=rowBits(material,y);
        for(int x=begin;x<end;++x) {
          const uint64_t bit=uint64_t(1)<<(x%64);
          if(fill) row[x/64]|=bit; else row[x/64]&=~bit;
        }
      });
    };
    draw(sheet,true);
    for(const auto& hole:sheet.children) draw(hole,false);
  }

  });
  return material;
}

// Read a mask word after applying a bit-level horizontal shift.
uint64_t shiftedMaskWord(const uint64_t* src, size_t srcWords, size_t wordIndex, int bitShift) {
  const uint64_t lo = wordIndex < srcWords ? src[wordIndex] : 0ULL;
  const uint64_t hi = wordIndex > 0 && wordIndex - 1 < srcWords ? src[wordIndex - 1] : 0ULL;
  return (lo << bitShift) | (hi >> (64 - bitShift));
}

// Check unaligned row collisions and stock containment.
bool rowsCollideAndInBoundsShiftedScalar(const uint64_t* occ,
                                         const uint64_t* material,
                                         const uint64_t* maskRow,
                                         size_t maskWords,
                                         int bitShift, size_t shiftedWords) {
  for (size_t i = 0; i < shiftedWords; ++i) {
    const uint64_t shiftedWord = shiftedMaskWord(maskRow, maskWords, i, bitShift);
    if ((shiftedWord & occ[i]) != 0ULL || (shiftedWord & ~material[i]) != 0ULL) {
      return true;
    }
  }
  return false;
}

// Merge an unaligned mask row into sheet occupancy.
void rowsOrShiftedScalar(uint64_t* occ, const uint64_t* maskRow, size_t maskWords, int bitShift, size_t shiftedWords) {
  for (size_t i = 0; i < shiftedWords; ++i) {
    occ[i] |= shiftedMaskWord(maskRow, maskWords, i, bitShift);
  }
}

// Check whether a translated mask fits stock without bitmap collisions.
bool maskFits(const BitmapGrid& occupancy,
              const BitmapGrid& material,
              const RasterMask& mask,
              int originX,
              int originY,
              bool useAvx2) {
  if (originX < 0 || originY < 0) {
    return false;
  }
  if (originX + mask.widthPx > occupancy.widthPx || originY + mask.heightPx > occupancy.heightPx) {
    return false;
  }

  const int wordShift = originX / 64;
  const int bitShift = originX % 64;
  const size_t neededWords = static_cast<size_t>((mask.widthPx + bitShift + 63) / 64);
  if (static_cast<size_t>(wordShift) + neededWords > occupancy.wordsPerRow) {
    return false;
  }

  for (int row : mask.collisionRows) {
    const uint64_t* occRow = rowBits(occupancy, originY + row) + wordShift;
    const uint64_t* matRow = rowBits(material, originY + row) + wordShift;
    if (bitShift == 0) {
      const uint64_t* maskRow = &mask.bits[static_cast<size_t>(row) * mask.wordsPerRow];
#if defined(__AVX2__)
      if (useAvx2) {
        if (rowsCollideAndInBoundsAvx2(occRow, matRow, maskRow, mask.wordsPerRow)) {
          return false;
        }
      } else
#endif
      {
        if (rowsCollideAndInBoundsScalar(occRow, matRow, maskRow, mask.wordsPerRow)) {
          return false;
        }
      }
    } else {
      const uint64_t* maskRow = &mask.bits[static_cast<size_t>(row) * mask.wordsPerRow];
      if (rowsCollideAndInBoundsShiftedScalar(occRow, matRow, maskRow, mask.wordsPerRow, bitShift, neededWords)) {
        return false;
      }
    }
  }
  return true;
}

// Commit an accepted mask to the sheet occupancy bitmap.
void applyMask(BitmapGrid& occupancy, const RasterMask& mask, int originX, int originY, bool useAvx2) {
  const int wordShift = originX / 64;
  const int bitShift = originX % 64;

  for (int row = 0; row < mask.heightPx; ++row) {
    uint64_t* occRow = rowBits(occupancy, originY + row) + wordShift;
    if (bitShift == 0) {
      const uint64_t* maskRow = &mask.bits[static_cast<size_t>(row) * mask.wordsPerRow];
#if defined(__AVX2__)
      if (useAvx2) {
        rowsOrAvx2(occRow, maskRow, mask.wordsPerRow);
      } else
#endif
      {
        rowsOrScalar(occRow, maskRow, mask.wordsPerRow);
      }
    } else {
      const uint64_t* maskRow = &mask.bits[static_cast<size_t>(row) * mask.wordsPerRow];
      rowsOrShiftedScalar(occRow, maskRow, mask.wordsPerRow, bitShift,
                          static_cast<size_t>((mask.widthPx + bitShift + 63) / 64));
    }
  }
}

// Identify one shape and precise orientation for raster caching.
std::string bitmapMaskCacheKey(const Polygon& part, double rotation) {
  std::ostringstream key;
  key<<polygonGeometryIdentity(part)<<'|'<<std::setprecision(17)<<rotation;
  return key.str();
}

// Combine geometry and permitted orientations for reusable searches.
std::string searchPolicyKey(const Polygon& part) {
  std::ostringstream key;
  key<<bitmapMaskCacheKey(part,part.rotation);
  if(!part.allowedAngles.empty()) {
    key<<"|allowed:"<<std::setprecision(17);
    for(double angle:part.allowedAngles) key<<angle<<';';
  }
  return key.str();
}

// Describe a repeated two-part row layout and its enumeration cursor.
struct PairPattern {
  Candidate a{0,0,0}, b{0,0,0};
  int width{0}, height{0}, columns{0}, rows{0}, members{1};
  uint64_t cursor{0};
  std::vector<Candidate> pending;
  std::vector<uint8_t> flags;
  size_t pendingCursor{0};
  std::vector<Candidate> group;
  size_t searchedMembers{0};
  int rowShift{0}, oddColumns{-1};
  bool interlocking{false};
  // Return the number of positions available in a repeated pattern.
  uint64_t capacity() const {
    const auto cells=oddColumns<0 ? uint64_t(columns)*rows :
        uint64_t(columns)*((rows+1)/2)+uint64_t(oddColumns)*(rows/2);
    return cells*members;
  }
  // Advance to the next available cached origin or pattern position.
  Candidate next() {
    const auto index = cursor++;
    const auto cell = index / members;
    auto c = !group.empty() ? group[size_t(index%members)] : (members == 2 && index % 2 ? b : a);
    if(oddColumns<0) {
      c.x += int(cell % columns) * width;
      c.y += int(cell / columns) * height;
    } else {
      const auto pair=cell/uint64_t(columns+oddColumns);
      const auto offset=cell%uint64_t(columns+oddColumns);
      const bool odd=offset>=uint64_t(columns);
      c.x+=int(odd?offset-columns:offset)*width+(odd?rowShift:0);
      c.y+=int(2*pair+odd)*height;
    }
    return c;
  }
};

// The raw NFP describes touching contours. With positive spacing nearly all
// of its vertices fail validation; propose the clearance offset instead.
std::vector<Point> clearanceContacts(const Polygon& nfp,const Config& config) {
  if(config.spacing==0) return nfp.points;
  Clipper2Lib::PathD path;
  for(const auto& p:nfp.points) path.emplace_back(p.x,p.y);
  const auto paths=Clipper2Lib::InflatePaths({path},config.spacing,
      Clipper2Lib::JoinType::Round,Clipper2Lib::EndType::Polygon,2.0,6,
      std::max(0.001,config.bitmapResolutionMm*0.1));
  std::vector<Point> points;
  for(const auto& contour:paths) for(const auto& p:contour) points.push_back({p.x,p.y,true});
  return points;
}

#include "interlocking_pattern.hpp"
#include "group_pattern_search.hpp"

// Begin with disjoint cells; continuous group trials also fit their contours.
PairPattern makePairPattern(const std::vector<const RasterMask*>& masks, int sheetW, int sheetH,
                            const Config& config, NfpCache& cache, RasterPixels& pixels,const SearchDeadline& deadline) {
  PairPattern best;
  std::unordered_map<size_t,PairPattern> groupSeeds;
  auto consider = [&](Candidate a, Candidate b, int members) {
    deadline.check();
    const auto& am = *masks[a.rotation];
    const auto& bm = *masks[b.rotation];
    const int minX = std::min(a.x,b.x), minY = std::min(a.y,b.y);
    a.x -= minX; b.x -= minX; a.y -= minY; b.y -= minY;
    const double maxGap=double(std::max(sheetW,sheetH))+1;
    const int gap=static_cast<int>(std::min(maxGap,std::ceil(std::max(config.spacing,config.holeSpacing)/config.bitmapResolutionMm)));
    const int w = std::max(a.x+am.widthPx, members==2 ? b.x+bm.widthPx : 0)+gap;
    const int h = std::max(a.y+am.heightPx, members==2 ? b.y+bm.heightPx : 0)+gap;
    if (w<=0 || h<=0 || w>sheetW || h>sheetH) return;
    const int edge=int(std::min(maxGap,std::ceil(config.sheetSpacing/config.bitmapResolutionMm)));
    PairPattern p{a,b,w,h,std::max(0,sheetW-2*edge+gap)/w,std::max(0,sheetH-2*edge+gap)/h,members};
    p.a.x+=edge;p.a.y+=edge;p.b.x+=edge;p.b.y+=edge;
    const size_t seedKey=(a.rotation*masks.size()+b.rotation)*2+size_t(members-1);
    if(config.bitmapGroupTrial) {
      const auto it=groupSeeds.find(seedKey);
      if(it!=groupSeeds.end() && !betterPattern(p,it->second)) return;
    } else if(!betterPattern(p,best)) return;
    if (members == 2) {
      const auto pa = shiftPolygon(am.rotatedPart,
          {a.x*config.bitmapResolutionMm-am.minX,a.y*config.bitmapResolutionMm-am.minY,true});
      const auto pb = shiftPolygon(bm.rotatedPart,
          {b.x*config.bitmapResolutionMm-bm.minX,b.y*config.bitmapResolutionMm-bm.minY,true});
      if (violatesPartClearance(pa,pb,config)) return;
      auto occupied = makeBitmapGrid(w,h);
      auto material = makeBitmapGrid(w,h);
      std::fill(material.bits.begin(),material.bits.end(),~uint64_t(0));
      pixels.ensure(am);
      applyMask(occupied,am,a.x,a.y,false);
      pixels.ensure(bm);
      if (!maskFits(occupied,material,bm,b.x,b.y,false)) return;
    }
    if(config.bitmapGroupTrial) groupSeeds[seedKey]=p;
    if(betterPattern(p,best)) best=p;
  };
  for (size_t a=0; a<masks.size(); ++a) consider({0,0,a},{0,0,a},1);
  // Bound preprocessing for inputs allowing many angles. The compact strategy
  // still searches every requested rotation.
  const size_t stride = std::max<size_t>(1,(masks.size()+7)/8);
  for (size_t a=0; a<masks.size(); a+=stride) for (size_t b=0; b<masks.size(); b+=stride) {
    deadline.check();
    const auto& am=*masks[a]; const auto& bm=*masks[b];
    const auto nfp=getOuterNfp(am.rotatedPart,bm.rotatedPart,false,config,cache);
    if (!nfp) continue;
    const auto contacts=clearanceContacts(*nfp,config);
    const size_t vertexStride=std::max<size_t>(1,(contacts.size()+127)/128);
    for (size_t i=0; i<contacts.size(); i+=vertexStride) {
      const auto& p=contacts[i];
      const int x=int(std::floor((p.x-bm.rotatedPart.points.front().x-am.minX+bm.minX)/config.bitmapResolutionMm));
      const int y=int(std::floor((p.y-bm.rotatedPart.points.front().y-am.minY+bm.minY)/config.bitmapResolutionMm));
      for(int dx:{0,1}) for(int dy:{0,1}) consider({0,0,a},{x+dx,y+dy,b},2);
    }
  }
  if(config.bitmapGroupTrial) {
    // Leave most of a bounded helper trial for placing the pattern and filling
    // its remaining gaps. Keep the last fully validated pattern on expiry.
    SearchDeadline preparation(config.timeLimitSeconds>0 ? std::min(2.0,config.timeLimitSeconds*0.4) : 0,
                               [&]{return deadline.expired();});
    auto seed=best;
    try {
      improveInterlockingPattern(best,masks,sheetW,sheetH,config,cache,preparation);
      // Rectangular capacity alone can discard the best interlocking motif.
      // Revisit alternative orientations across successive continuous trials.
      std::vector<std::pair<size_t,PairPattern>> alternatives(groupSeeds.begin(),groupSeeds.end());
      std::sort(alternatives.begin(),alternatives.end(),[](const auto& a,const auto& b){return a.first<b.first;});
      for(size_t i=0;i<std::min<size_t>(3,alternatives.size());++i) {
        auto candidate=alternatives[(config.searchIteration*3+i)%alternatives.size()].second;
        const auto original=candidate;
        SearchDeadline candidateBudget(0.25,[&]{return preparation.expired();});
        try {improveInterlockingPattern(candidate,masks,sheetW,sheetH,config,cache,candidateBudget);}
        catch(const SearchTimeExpired&) {preparation.check();}
        if(betterPattern(candidate,best)) {best=std::move(candidate);seed=original;}
      }
      improveGroupPattern(best,seed,masks,sheetW,sheetH,config,cache,preparation);
    } catch(const SearchTimeExpired&) { deadline.check(); }
  }
  return best;
}

// Search and validate part placements on one stock sheet.
PlacementResult placePartsBitmapOnSingleSheet(const Polygon& sheet,
                                              const std::vector<Polygon>& parts,
                                              const Config& config,
                                              ParallelLoop& parallel,
                                              detail::RejectionCacheBudget& rejectionBudget,
                                              std::unique_ptr<GpuBitmap>& gpu,
                                              const SearchDeadline& deadline,
                                              bool useAvx2,
                                              BitmapNestingStats* stats,
                                              const BitmapPartProgressCallback& onPartProgress,
                                              const std::vector<std::pair<Placement,Polygon>>& seeds = {}) {
  PlacementResult out;
  out.totalarea = polygonMaterialArea(sheet);

  Bounds sheetBounds;
  BitmapNestingStats localStats;
  const auto preparationStart=std::chrono::steady_clock::now();
  BitmapGrid material;
  try { material=rasterizeSheetMaterial(sheet, config.bitmapResolutionMm, sheetBounds,deadline,parallel); }
  catch(const SearchTimeExpired&) {
    out.unplaced=parts;
    localStats.timeLimitReached=true;
    if(stats) *stats=localStats;
    return out;
  }
  BitmapGrid occupancy = makeBitmapGrid(material.widthPx, material.heightPx);
  detail::FreeRectangles freeRectangles(material.widthPx,material.heightPx);
  detail::FreeRectangles clearanceRectangles(material.widthPx,material.heightPx);
  // Bounding boxes supply inexpensive MaxRects-style gap proposals only.
  // They never reject a concave placement or an insert into a hole.
  detail::FreeRectangles proposalSpaces(material.widthPx,material.heightPx);
  auto occupyProposalSpace=[&](int x,int y,int w,int h) {
    const int gap=int(std::min(double(std::max(material.widthPx,material.heightPx)),std::ceil(config.spacing/config.bitmapResolutionMm)));
    proposalSpaces.occupy({x-gap,y-gap,w+2*gap,h+2*gap});
  };
  std::vector<Bounds> sheetHoleBounds;
  for(const auto& hole:sheet.children) sheetHoleBounds.push_back(getPolygonBounds(hole.points));

  // Heap-own masks so pointers in rotationMasks/rasterPlacements stay valid
  // even when the lookup table grows to thousands of requested angles.
  std::unordered_map<std::string, std::unique_ptr<RasterMask>> maskCache;
  std::unordered_map<std::string, std::vector<const RasterMask*>> rotationCache;
  RasterPixels pixels(config.bitmapResolutionMm,deadline,parallel);
  NfpCache contactNfpCache;
  std::vector<std::unordered_map<NfpKey, std::vector<Point>, NfpKeyHash>> workerContacts(parallel.size());
  std::vector<std::vector<Candidate>> workerCandidates(parallel.size());
  std::vector<size_t> workerSkipped(parallel.size());
  std::vector<Polygon> placedAbsolute;
  std::vector<Bounds> placedBounds;
  // Absolute contours remain stable even when the placed-part vector grows.
  struct HolePocket { Polygon contour; Bounds bounds; };
  std::vector<HolePocket> holePockets;
  std::unordered_map<std::string,size_t> exhaustedPockets;
  std::vector<Placement> placements;
  SpatialIndex neighbours(std::max(config.bitmapResolutionMm,
      std::max(sheetBounds.width, sheetBounds.height) / 32.0));
  std::vector<RasterPlacement> rasterPlacements;
  std::unordered_map<std::string, std::vector<RasterPlacement>> repeated;
  std::unordered_set<std::string> exhausted;
  std::unordered_map<std::string,size_t> failedContactVersions;
  std::unordered_map<std::string, uint64_t> fineSearchCursor;
  std::unordered_map<std::string, RejectedOrigins> rejectedOrigins;
  std::unordered_map<std::string, size_t> repetitions;
  std::unordered_map<std::string, PairPattern> patterns;
  if (config.bitmapPatternTrial)
    for (const auto& p : parts) ++repetitions[searchPolicyKey(p)];
  int usedWidth = 0, usedHeight = 0;
  size_t placedCount = 0;
  size_t unplacedCount = 0;
  const size_t totalParts = parts.size();
  const int searchStep = std::max(1, config.bitmapSearchStepPx);
  bool gpuSheetUploaded=false;
  bool gpuAvailable=config.gpuEnabled;
  auto gpuFailed=[&](const std::exception& e) {
    if(!config.gpuFallbackToCpu) throw std::runtime_error(e.what());
    localStats.gpuFallbackReason=e.what();
    gpuAvailable=false;
    gpu.reset();
  };
  std::string gpuMaskIdentity;
  size_t gpuOccupancyVersion=std::numeric_limits<size_t>::max();
  const auto bitmapStart = std::chrono::steady_clock::now();
  localStats.phases.preparationMs=std::chrono::duration<double,std::milli>(bitmapStart-preparationStart).count();

  // Refinement moves polygons off the old raster. Rebuild occupancy on the
  // sheet's pixel grid; never reuse rejected origins or masks from before a move.
  std::vector<std::unique_ptr<RasterMask>> seedMasks;
  try {
    for(const auto& [placement,absolute]:seeds) {
      deadline.check();
      auto mask=std::make_unique<RasterMask>(rasterizePartMask(absolute,0,config.bitmapResolutionMm,config.curveTolerance));
      const auto b=getPolygonBounds(absolute.points);
      const int x=int(std::floor((b.x-sheetBounds.x)/config.bitmapResolutionMm));
      const int y=int(std::floor((b.y-sheetBounds.y)/config.bitmapResolutionMm));
      mask->minX=sheetBounds.x+x*config.bitmapResolutionMm;
      mask->minY=sheetBounds.y+y*config.bitmapResolutionMm;
      mask->widthPx=rasterDimension(b.x+b.width-mask->minX,config.bitmapResolutionMm);
      mask->heightPx=rasterDimension(b.y+b.height-mask->minY,config.bitmapResolutionMm);
      mask->wordsPerRow=size_t((mask->widthPx+63)/64);
      mask->contacts.clear();
      for(const auto& p:absolute.points) mask->contacts.push_back({(p.x-mask->minX)/config.bitmapResolutionMm,(p.y-mask->minY)/config.bitmapResolutionMm,true});
      pixels.ensure(*mask);
      applyMask(occupancy,*mask,x,y,useAvx2);
      occupyProposalSpace(x,y,mask->widthPx,mask->heightPx);
      auto core=mask->filledCore;core.x+=x;core.y+=y;
      if(int64_t(core.width)*core.height>=int64_t(mask->widthPx)*mask->heightPx/2) freeRectangles.occupy(core);
      usedWidth=std::max(usedWidth,x+mask->widthPx);
      usedHeight=std::max(usedHeight,y+mask->heightPx);
      neighbours.insert(b,placedAbsolute.size());
      placedAbsolute.push_back(absolute);placedBounds.push_back(b);
      placements.push_back(placement);
      // Seed geometry is absolute, so do not share its pair-pattern identity
      // with masks rotated about the source origin.
      rasterPlacements.push_back({x,y,mask.get(),{}});
      for(const auto& hole:absolute.children) holePockets.push_back({hole,getPolygonBounds(hole.points)});
      out.area+=polygonMaterialArea(absolute);
      seedMasks.push_back(std::move(mask));
    }
  } catch(const SearchTimeExpired&) {
    localStats.timeLimitReached=true;
  }
  placedCount=placements.size();
  localStats.placedParts=placedCount;

  for (size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
    if(deadline.expired()) {
      localStats.timeLimitReached=true;
      out.unplaced.insert(out.unplaced.end(),parts.begin()+partIndex,parts.end());
      break;
    }
    const Polygon& part = parts[partIndex];
    const auto partStart = std::chrono::steady_clock::now();

    std::optional<Placement> acceptedPlacement;
    const RasterMask* acceptedMask = nullptr;
    int acceptedX = 0, acceptedY = 0;
    size_t partCandidatesExamined = 0, partBoundaryRejects = 0;
    size_t partBitmapCollisions = 0, partVectorValidationRejects = 0;
    Polygon acceptedAbsolute;
    Bounds acceptedBounds;
    const std::string identity = polygonGeometryIdentity(part);
    const std::string searchIdentity = searchPolicyKey(part);
    try {
    const auto failed=failedContactVersions.find(searchIdentity);
    if(config.bitmapBottomLeft && config.bitmapCacheRejects && failed!=failedContactVersions.end() && failed->second==placedCount) {
      ++localStats.failedSearchSkips;
    } else {

    const int rotationCount = int(part.allowedAngles.size());
    auto [rotationIt,newRotations]=rotationCache.try_emplace(searchIdentity);
    auto& rotationMasks=rotationIt->second;
    if(newRotations) {
      PhaseTimer timer{localStats.phases.preparationMs};
      rotationMasks.reserve(static_cast<size_t>(rotationCount));
      for (int r = 0; r < rotationCount; ++r) {
        deadline.check();
        const auto angleIndex=(size_t(r)+size_t(config.searchIteration%uint64_t(rotationCount)))%size_t(rotationCount);
        const double rotation = part.allowedAngles[angleIndex];
        const std::string key = bitmapMaskCacheKey(part,rotation);
        auto it = maskCache.find(key);
        if (it == maskCache.end()) {
          it = maskCache.emplace(key, std::make_unique<RasterMask>(
              rasterizePartMask(part, rotation, config.bitmapResolutionMm, config.curveTolerance))).first;
        }
        rotationMasks.push_back(it->second.get());
      }
    }

    int globalMaxOriginX = std::numeric_limits<int>::min();
    int globalMaxOriginY = std::numeric_limits<int>::min();
    for (const RasterMask* mask : rotationMasks) {
      globalMaxOriginX = std::max(globalMaxOriginX, material.widthPx - mask->widthPx);
      globalMaxOriginY = std::max(globalMaxOriginY, material.heightPx - mask->heightPx);
    }
    if(rotationMasks.size()<=16) {
      PhaseTimer timer{localStats.phases.preparationMs};
      // Keep dense angular searches lazy: proving a rejection must not force
      // hundreds of otherwise unused masks into the bounded pixel cache.
      bool possible=false;
      for(const auto* mask:rotationMasks) {
        pixels.ensure(*mask);
        if(!mask->coreVerified) {
          auto core=mask->filledCore;
          // Pixel centres alone cannot prove continuous material containment.
          // Verify the rectangle before using it to rule out clearance gaps.
          for(int attempt=0;attempt<2 && core.width>0 && core.height>0;++attempt) {
            const double x=mask->minX+core.x*config.bitmapResolutionMm,y=mask->minY+core.y*config.bitmapResolutionMm;
            const double w=core.width*config.bitmapResolutionMm,h=core.height*config.bitmapResolutionMm;
            Polygon box;box.points={{x,y,true},{x+w,y,true},{x+w,y+h,true},{x,y+h,true}};
            if(!hasMaterialOutsideSheet(box,mask->rotatedPart,config)) {mask->geometricCore=core;break;}
            ++core.x;++core.y;core.width-=2;core.height-=2;
          }
          mask->coreVerified=true;
        }
        if(freeRectangles.fits(mask->filledCore.width,mask->filledCore.height) &&
           clearanceRectangles.fits(mask->geometricCore.width,mask->geometricCore.height)) possible=true;
      }
      if(!possible && !rasterPlacements.empty()) exhausted.insert(searchIdentity);
    }

    auto prepareGpu=[&] {
      deadline.check();
      if(!gpu) {
        gpu=std::make_unique<GpuBitmap>(config.gpuDevice);
      }
      localStats.gpuDevice=gpu->device().name;
      if(!gpuSheetUploaded) {
        gpu->setSheet(material.widthPx,material.heightPx,material.bits);
        gpuSheetUploaded=true;
      }
      if(gpuMaskIdentity!=searchIdentity) {
        uint64_t total=0;
        for(const auto* mask:rotationMasks) total+=mask->wordsPerRow*uint64_t(mask->heightPx);
        if(total>64ULL*1024*1024) throw std::runtime_error("Rotated masks exceed the 512 MiB OpenCL buffer budget");
        std::vector<uint64_t> words; words.reserve(size_t(total));
        std::vector<GpuMaskInfo> masks; masks.reserve(rotationMasks.size());
        for(const auto* mask:rotationMasks) {
          pixels.ensure(*mask);
          masks.push_back({uint32_t(mask->widthPx),uint32_t(mask->heightPx),uint32_t(mask->wordsPerRow),uint32_t(words.size())});
          words.insert(words.end(),mask->bits.begin(),mask->bits.end());
        }
        gpu->setMasks(masks,words);
        gpuMaskIdentity=searchIdentity;
      }
      if(gpuOccupancyVersion!=placedCount) {
        gpu->setOccupancy(occupancy.bits);
        gpuOccupancyVersion=placedCount;
      }
    };
    auto gpuGrid=[&](uint64_t first,uint32_t count,uint32_t rows,uint32_t step,uint32_t w,uint32_t h)
        ->std::optional<std::vector<uint8_t>> {
      if(!gpuAvailable) return std::nullopt;
      PhaseTimer timer{localStats.phases.gpuMs};
      try {
        prepareGpu();
        auto flags=gpu->filterGrid(first,count,rows,step,w,h);
        ++localStats.gpuBatches; localStats.gpuCandidates+=count;
        deadline.check();
        return flags;
      } catch(const std::exception& e) { gpuFailed(e); return std::nullopt; }
    };
    auto gpuCandidates=[&](std::span<const Candidate> candidates)
        ->std::optional<std::vector<uint8_t>> {
      if(!gpuAvailable || candidates.empty()) return std::nullopt;
      PhaseTimer timer{localStats.phases.gpuMs};
      try {
        prepareGpu();
        std::vector<GpuCandidate> input; input.reserve(candidates.size());
        for(const auto& c:candidates) input.push_back({c.x,c.y,uint32_t(c.rotation)});
        auto flags=gpu->filter(input);
        ++localStats.gpuBatches; localStats.gpuCandidates+=input.size();
        deadline.check();
        return flags;
      } catch(const std::exception& e) { gpuFailed(e); return std::nullopt; }
    };
    auto& rejected = rejectedOrigins[searchIdentity];
    if (rejected.rows == 0 && globalMaxOriginX >= 0 && globalMaxOriginY >= 0) {
      rejected.rows = uint64_t(globalMaxOriginY) + 1;
      rejected.rotations = rotationMasks.size();
      rejected.size = (uint64_t(globalMaxOriginX) + 1) * rejected.rows * rejected.rotations;
      rejected.configure(config.bitmapCacheRejects ? &rejectionBudget : nullptr);
    }
    using Score = std::tuple<int64_t, int, int, int, size_t>;
    auto score = [&](const Candidate& c) -> Score {
      const auto* m = rotationMasks[c.rotation];
      const int w = std::max(usedWidth, c.x + m->widthPx);
      const int h = std::max(usedHeight, c.y + m->heightPx);
      // Restarting identical copies in a different order changes nothing.
      // Explore a different permitted anchor angle before greedy completion.
      const int64_t anchorPenalty=placedCount==0 && config.searchIteration>0 && c.rotation!=0
          ? int64_t(material.widthPx)*material.heightPx+material.widthPx+material.heightPx+1 : 0;
      if(config.bitmapBottomLeft) return {anchorPenalty+int64_t(c.y),c.x,0,0,c.rotation};
      return {anchorPenalty+int64_t(w) * h, std::max(w, h), c.x, c.y, c.rotation};
    };
    std::optional<Score> bestScore;
    // Workers read one immutable occupancy snapshot. Cache writes, ranking and
    // placement commits stay on the caller, preserving serial candidate order.
    auto evaluateBatch = [&](std::span<const Candidate> candidates,bool first,
                             std::span<const uint8_t> cachedFlags=std::span<const uint8_t>{},
                             bool freshFlags=false,bool allowGpu=true,bool select=true) -> std::optional<size_t> {
      deadline.check();
      enum class State { Pending, Skip, Boundary, Cached, Bitmap, Geometry, Pocket, Valid };
      struct Check { State state{State::Pending}; Polygon absolute; Bounds bounds; size_t neighbours{0}; };
      std::vector<Check> checks(candidates.size());
      std::vector<Candidate> eligible;
      std::vector<size_t> indices;
      for(size_t i=0;i<candidates.size();++i) {
        const auto& c=candidates[i]; const auto* m=rotationMasks[c.rotation];
        auto& check=checks[i];
        if(c.x<0 || c.y<0 || c.x>material.widthPx-m->widthPx || c.y>material.heightPx-m->heightPx)
          check.state=State::Boundary;
        else if(bestScore && score(c)>=*bestScore) check.state=State::Skip;
        else if(rejected.contains(rejected.index(c.x,c.y,c.rotation))) check.state=State::Cached;
        else if(!cachedFlags.empty() && !cachedFlags[i]) check.state=State::Bitmap;
        else { eligible.push_back(c); indices.push_back(i); }
      }
      auto flags=allowGpu ? gpuCandidates(eligible) : std::optional<std::vector<uint8_t>>{};
      if(flags) {
        for(size_t j=0;j<indices.size();++j) if(!(*flags)[j]) checks[indices[j]].state=State::Bitmap;
      } else if(!freshFlags || cachedFlags.empty()) {
        // Pin a memory-bounded group of angles, then distribute candidates
        // across workers. A dense angle grid often has only one origin per
        // angle in a batch: a separate parallel loop per angle ran serially.
        std::vector<std::vector<size_t>> byRotation(rotationMasks.size());
        for(size_t i:indices) byRotation[candidates[i].rotation].push_back(i);
        std::vector<const RasterMask*> pinned;
        std::vector<size_t> group;
        size_t pinnedBytes=0;
        auto checkPinned=[&] {
          parallel.run(group.size(),[&](size_t begin,size_t end,size_t) {
            for(size_t j=begin;j<end;++j) {
              deadline.check(); const size_t i=group[j]; const auto& c=candidates[i];
              if(!maskFits(occupancy,material,*rotationMasks[c.rotation],c.x,c.y,useAvx2)) checks[i].state=State::Bitmap;
            }
          });
          localStats.candidateWorkersUsed=std::max(localStats.candidateWorkersUsed,std::min(parallel.size(),group.size()));
          pinned.clear();group.clear();pinnedBytes=0;
        };
        for(size_t r=0;r<byRotation.size();++r) if(!byRotation[r].empty()) {
          const auto* mask=rotationMasks[r];
          const size_t bytes=mask->wordsPerRow*size_t(mask->heightPx)*sizeof(uint64_t)+size_t(mask->heightPx)*sizeof(int);
          if(!pinned.empty() && pinnedBytes+bytes>64ULL*1024*1024) checkPinned();
          pixels.ensure(*mask,true,pinned);
          pinned.push_back(mask);pinnedBytes+=bytes;
          group.insert(group.end(),byRotation[r].begin(),byRotation[r].end());
        }
        if(!group.empty()) checkPinned();
      }
      std::vector<size_t> exact;
      for(size_t i:indices) if(checks[i].state==State::Pending) exact.push_back(i);
      std::vector<uint8_t> exactWorkers(parallel.size(),0);
      bool interrupted=false;
      try { parallel.run(exact.size(),[&](size_t begin,size_t end,size_t slot) {
        for(size_t j=begin;j<end;++j) {
          deadline.check(); const size_t i=exact[j]; const auto& c=candidates[i]; auto& check=checks[i];
          exactWorkers[slot]=1;
          const auto* mask=rotationMasks[c.rotation];
          const double shiftX=sheetBounds.x+c.x*config.bitmapResolutionMm-mask->minX;
          const double shiftY=sheetBounds.y+c.y*config.bitmapResolutionMm-mask->minY;
          check.bounds={sheetBounds.x+c.x*config.bitmapResolutionMm,sheetBounds.y+c.y*config.bitmapResolutionMm,
                        mask->rotatedBounds.width,mask->rotatedBounds.height};
          check.absolute=shiftPolygon(mask->rotatedPart,{shiftX,shiftY,true});
          if(c.pocket && hasMaterialOutsideSheet(check.absolute,*c.pocket,config)) {
            check.state=State::Pocket; continue;
          }
          if(config.bitmapValidateGeometry || config.spacing>0 || config.sheetSpacing>0 || config.holeSpacing>0) {
            if(violatesSheetClearance(check.absolute,sheet,config)) { check.state=State::Geometry; continue; }
            const double gap=std::min(std::max(config.spacing,config.holeSpacing),std::max(sheetBounds.width,sheetBounds.height));
            const Bounds expanded{check.bounds.x-gap,check.bounds.y-gap,check.bounds.width+2*gap,check.bounds.height+2*gap};
            for(size_t id:neighbours.query(expanded)) {
              if(!boundsIntersect(expanded,placedBounds[id])) continue;
              ++check.neighbours;
              if(violatesPartClearance(check.absolute,placedAbsolute[id],config)) { check.state=State::Geometry; break; }
            }
          }
          if(check.state==State::Pending) check.state=State::Valid;
        }
      }); } catch(const SearchTimeExpired&) { interrupted=true; }
      localStats.candidateWorkersUsed=std::max(localStats.candidateWorkersUsed,
          size_t(std::count(exactWorkers.begin(),exactWorkers.end(),uint8_t(1))));
      std::optional<size_t> selected;
      for(size_t i=0;i<candidates.size();++i) {
        const auto& c=candidates[i]; auto& check=checks[i];
        if(check.state==State::Pending) continue;
        ++partCandidatesExamined; localStats.neighbourGeometryChecks+=check.neighbours;
        if(check.state==State::Boundary) ++partBoundaryRejects;
        else if(check.state==State::Cached) ++localStats.rejectedPositionSkips;
        else if(check.state==State::Bitmap || check.state==State::Geometry) {
          rejected.insert(rejected.index(c.x,c.y,c.rotation));
          if(check.state==State::Bitmap) ++partBitmapCollisions; else ++partVectorValidationRejects;
        } else if(select && check.state==State::Valid && !(first && selected) && (!bestScore || score(c)<*bestScore)) {
          const auto* mask=rotationMasks[c.rotation];
          bestScore=score(c); selected=i;
          acceptedX=c.x; acceptedY=c.y; acceptedMask=mask;
          acceptedAbsolute=std::move(check.absolute); acceptedBounds=check.bounds;
          acceptedPlacement=Placement{sheetBounds.x+c.x*config.bitmapResolutionMm-mask->minX,
              sheetBounds.y+c.y*config.bitmapResolutionMm-mask->minY,part.id,mask->rotationDeg,part.source,part.filename,0.0,{}};
        }
      }
      if(interrupted) throw SearchTimeExpired{};
      return selected;
    };
    const size_t validationBatch=64;
    // Keep the GPU busy with a large immutable occupancy snapshot, but retain
    // CPU validation order and small exact-geometry chunks. No placement is
    // committed until this ordered search returns.
    auto evaluateOrdered = [&](std::span<const Candidate> ordered,bool first) {
      const size_t uploadBatch=gpuAvailable ? size_t(config.gpuBatchSize) : validationBatch;
      for(size_t offset=0;offset<ordered.size();offset+=uploadBatch) {
        deadline.check();
        const auto chunk=ordered.subspan(offset,std::min(uploadBatch,ordered.size()-offset));
        const auto flags=gpuAvailable ? gpuCandidates(chunk) : std::optional<std::vector<uint8_t>>{};
        for(size_t i=0;i<chunk.size();i+=validationBatch) {
          const auto group=chunk.subspan(i,std::min(validationBatch,chunk.size()-i));
          const auto allowed=flags ? std::span<const uint8_t>(*flags).subspan(i,group.size()) : std::span<const uint8_t>{};
          if(evaluateBatch(group,first,allowed,bool(flags),false) && first) return true;
        }
      }
      return false;
    };

    int maxWidth = 1, maxHeight = 1;
    for (const auto* m : rotationMasks) {
      maxWidth = std::max(maxWidth, m->widthPx);
      maxHeight = std::max(maxHeight, m->heightPx);
    }
    bool fromPattern=false;
    if (config.bitmapPatternTrial && exhaustedPockets[searchIdentity]<holePockets.size()) {
      PhaseTimer timer{localStats.phases.searchMs};
      // Try existing cavities before consuming more sheet area. Bounds only
      // propose origins; exact containment and normal clearance checks decide.
      // Visit every pocket at one angle before trying another angle. Repeated
      // inserts can then advance to a free hole without exhausting 360 angles
      // in each already occupied pocket.
      std::vector<Candidate> pockets;
      auto flushPockets=[&] {
        if(pockets.empty()) return;
        if(evaluateBatch(pockets,true)) { fromPattern=true; ++localStats.holePlacements; }
        pockets.clear();
      };
      for(size_t r=0;r<rotationMasks.size() && !fromPattern;++r) {
        deadline.check();
        for(size_t h=exhaustedPockets[searchIdentity];h<holePockets.size();++h) {
          const auto& pocket=holePockets[h];
          deadline.check();
          const auto* m=rotationMasks[r];
          const double gap=config.holeSpacing;
          const double left=(pocket.bounds.x+gap-sheetBounds.x)/config.bitmapResolutionMm;
          const double bottom=(pocket.bounds.y+gap-sheetBounds.y)/config.bitmapResolutionMm;
          const double right=(pocket.bounds.x+pocket.bounds.width-gap-sheetBounds.x-m->rotatedBounds.width)/config.bitmapResolutionMm;
          const double top=(pocket.bounds.y+pocket.bounds.height-gap-sheetBounds.y-m->rotatedBounds.height)/config.bitmapResolutionMm;
          if(left>right || bottom>top) continue;
          const int xs[]={int(std::floor((left+right)/2)),int(std::ceil(left)),int(std::floor(right))};
          const int ys[]={int(std::floor((bottom+top)/2)),int(std::ceil(bottom)),int(std::floor(top))};
          for(int x:xs) {
            for(int y:ys) {
              if(x<left || x>right || y<bottom || y>top) continue;
              if(rejected.contains(rejected.index(x,y,r))) continue;
              Candidate c{x,y,r}; c.pocket=&pocket.contour; pockets.push_back(c);
              if(pockets.size()==validationBatch) flushPockets();
              if(fromPattern) break;
            }
            if(fromPattern) break;
          }
          if(fromPattern) break;
        }
      }
      if(!fromPattern) flushPockets();
      // Every origin/angle in these cavities has failed for this exact shape
      // and angle policy. Occupancy only grows, so only newly added cavities
      // can become useful to a later identical copy.
      if(!fromPattern) exhaustedPockets[searchIdentity]=holePockets.size();
    }
    if (!config.bitmapBottomLeft && !fromPattern && config.bitmapPatternTrial && repetitions[searchIdentity] >= 6) {
      auto [patternIt,inserted] = patterns.try_emplace(searchIdentity);
      if (inserted) patternIt->second=makePairPattern(rotationMasks,material.widthPx,material.heightPx,config,contactNfpCache,pixels,deadline);
      auto& pattern=patternIt->second;
      localStats.maxPatternGroupSize=std::max(localStats.maxPatternGroupSize,pattern.searchedMembers);
      if(pattern.interlocking) localStats.maxInterlockingCapacity=std::max(localStats.maxInterlockingCapacity,size_t(pattern.capacity()));
      while(!fromPattern) {
        if(pattern.pendingCursor==pattern.pending.size()) {
          pattern.pending.clear(); pattern.pendingCursor=0;
          while(pattern.cursor<pattern.capacity() && pattern.pending.size()<validationBatch) pattern.pending.push_back(pattern.next());
          if(pattern.pending.empty()) break;
          pattern.flags=gpuCandidates(pattern.pending).value_or(std::vector<uint8_t>{});
          // Prevalidate lookahead once across CPU workers. Permanent failures
          // enter the rejection cache; positives get a fresh check at commit.
          evaluateBatch(pattern.pending,false,pattern.flags,!pattern.flags.empty(),false,false);
        }
        // GPU negatives remain invalid as occupancy grows. Cached positives
        // are checked again on CPU against all placements committed since upload.
        const auto pending=std::span<const Candidate>(pattern.pending).subspan(pattern.pendingCursor);
        const auto batch=pending.first(1);
        const auto flags=pattern.flags.empty() ? std::span<const uint8_t>{}
            : std::span<const uint8_t>(pattern.flags).subspan(pattern.pendingCursor,batch.size());
        const auto found=evaluateBatch(batch,true,flags,false,false);
        pattern.pendingCursor+=found ? *found+1 : batch.size();
        if(found) { fromPattern=true; ++localStats.patternPlacements; }
      }
    }
    std::vector<Candidate> candidates;
    if (!fromPattern && !exhausted.contains(searchIdentity)) {
      PhaseTimer proposalTimer{localStats.phases.proposalsMs};
      const size_t tiles=std::min(parallel.size(),std::max<size_t>(1,rasterPlacements.size()));
      for(auto& batch:workerCandidates) batch.clear();
      std::fill(workerSkipped.begin(),workerSkipped.end(),0);
      parallel.run(rotationMasks.size()*tiles,[&](size_t begin,size_t end,size_t slot) {
      auto& localCandidates=workerCandidates[slot];
      localCandidates.clear();
      auto& pairContacts=workerContacts[slot];
      auto& skipped=workerSkipped[slot];
      skipped=0;
      auto propose = [&](int x, int y, size_t r) {
      if((localCandidates.size() & 1023)==0) deadline.check();
      const auto* m = rotationMasks[r];
      if (x >= 0 && y >= 0 && x <= material.widthPx - m->widthPx &&
          y <= material.heightPx - m->heightPx) {
        const auto origin = rejected.index(x, y, r);
        if (rejected.contains(origin)) { ++skipped; return; }
        Candidate c{x,y,r};
        const auto s = score(c);
        c.area = std::get<0>(s); c.extent = std::get<1>(s);
        localCandidates.push_back(c);
      }
    };
      // Fine angle grids must not multiply expensive contact NFP work by 3600.
      // Every angle still gets origin/bounds proposals, collision tests and
      // local refinement; exact pair contacts use a bounded angular sample.
      const size_t contactStride = std::max<size_t>(1, (rotationMasks.size() + 63) / 64);
      const size_t frontierStart = rotationMasks.size() > 64 && rasterPlacements.size() > 24
          ? rasterPlacements.size() - 24 : 0;
      for (size_t job = begin; job < end; ++job) {
        const size_t r=job/tiles,tile=job%tiles;
        deadline.check();
        const size_t first=localCandidates.size();
        const auto* m = rotationMasks[r];
        const double maxGap=double(std::max(material.widthPx,material.heightPx))+1;
        const int edgeGap=static_cast<int>(std::min(maxGap,std::ceil(config.sheetSpacing/config.bitmapResolutionMm)));
        const int partGap=static_cast<int>(std::min(maxGap,std::ceil(config.spacing/config.bitmapResolutionMm)));
        if(tile==0) {
          const int farX=material.widthPx-m->widthPx-edgeGap;
          const int farY=material.heightPx-m->heightPx-edgeGap;
          propose(edgeGap,edgeGap,r);
          propose(farX,edgeGap,r); propose(edgeGap,farY,r); propose(farX,farY,r);
          for(const auto& space:proposalSpaces.regions()) {
            const int left=std::max(edgeGap,space.x),bottom=std::max(edgeGap,space.y);
            const int right=std::min(farX,space.x+space.width-m->widthPx);
            const int top=std::min(farY,space.y+space.height-m->heightPx);
            if(left<=right && bottom<=top) {
              propose(left,bottom,r);propose(right,bottom,r);
              propose(left,top,r);propose(right,top,r);
            }
          }
          // Stock cutouts can block every part-to-part contact while leaving
          // entire strips free. Seed the strips beside their bounds directly;
          // all proposals still pass bitmap and exact clearance validation.
          const double holeGap=std::min(maxGap,config.holeSpacing/config.bitmapResolutionMm);
          auto pixel=[&](double value) { return int(std::clamp(value,-maxGap,maxGap)); };
          for(const auto& hole:sheetHoleBounds) {
            const int left=pixel(std::floor((hole.x-sheetBounds.x)/config.bitmapResolutionMm-holeGap))-m->widthPx;
            const int right=pixel(std::ceil((hole.x+hole.width-sheetBounds.x)/config.bitmapResolutionMm+holeGap));
            const int bottom=pixel(std::floor((hole.y-sheetBounds.y)/config.bitmapResolutionMm-holeGap))-m->heightPx;
            const int top=pixel(std::ceil((hole.y+hole.height-sheetBounds.y)/config.bitmapResolutionMm+holeGap));
            for(int x:{left,right}) { propose(x,edgeGap,r); propose(x,farY,r); }
            for(int y:{bottom,top}) { propose(edgeGap,y,r); propose(farX,y,r); }
          }
        }
        const size_t frontierCount=rasterPlacements.size()-frontierStart;
        for (size_t i = frontierStart+frontierCount*tile/tiles; i < frontierStart+frontierCount*(tile+1)/tiles; ++i) {
          const auto& q = rasterPlacements[i];
          const int right = q.x + q.mask->widthPx, top = q.y + q.mask->heightPx;
          // Bounding-box contacts cheaply grow rows and columns around the occupied region.
          propose(right, q.y, r); propose(q.x, top, r);
          propose(right+partGap, q.y, r); propose(q.x, top+partGap, r);
          // Contact with one panel and alignment with another edge can expose
          // a channel that the original eight bounding-box contacts missed.
          propose(right+partGap,edgeGap,r); propose(edgeGap,top+partGap,r);
          propose(right+partGap,top-m->heightPx,r);
          propose(right-m->widthPx,top+partGap,r);
          propose(q.x-m->widthPx-partGap,q.y,r); propose(q.x,q.y-m->heightPx-partGap,r);
          propose(right - m->widthPx, top, r); propose(right, top - m->heightPx, r);
          propose(q.x - m->widthPx, q.y, r); propose(q.x, q.y - m->heightPx, r);
          // A bounded recent frontier adds interlocking proposals for concave parts.
          if ((config.bitmapNfpContacts || !config.bitmapBottomLeft || !seeds.empty()) && i + 24 >= rasterPlacements.size() && r % contactStride == 0) {
            if (q.identity == identity || config.bitmapNfpContacts) {
              // Compute tight pair contacts once per repeated shape/rotation pair.
              // Unlike full NFP placement, no union of all occupied NFPs is built.
              NfpKey key{q.identity, identity, q.mask->rotationDeg, m->rotationDeg, false};
              auto [it, inserted] = pairContacts.try_emplace(key);
              if (inserted) {
                auto nfp = getOuterNfp(q.mask->rotatedPart, m->rotatedPart, false, config, contactNfpCache);
                if (nfp) {
                  for (const auto& p : clearanceContacts(*nfp,config))
                    it->second.push_back({
                      (p.x - m->rotatedPart.points.front().x - q.mask->minX + m->minX) / config.bitmapResolutionMm,
                      (p.y - m->rotatedPart.points.front().y - q.mask->minY + m->minY) / config.bitmapResolutionMm, true});
                }
              }
              for (const auto& offset : it->second) {
                const int x = static_cast<int>(std::floor(q.x + offset.x));
                const int y = static_cast<int>(std::floor(q.y + offset.y));
                for (int dx : {0, 1}) for (int dy : {0, 1}) propose(x + dx, y + dy, r);
              }
            } else {
              for (const auto& a : q.mask->contacts)
                for (const auto& b : m->contacts)
                  propose(static_cast<int>(std::llround(q.x + a.x - b.x)),
                          static_cast<int>(std::llround(q.y + a.y - b.y)), r);
            }
          }
        }
        auto it = repeated.find(identity);
        if (tile==0 && it != repeated.end() && it->second.size() >= 2) {
          const auto& copies = it->second;
          const auto& last = copies.back();
          // Reuse successful relative pair translations; each proposal is revalidated.
          for (size_t j = copies.size() > 9 ? copies.size() - 9 : 1; j < copies.size(); ++j) {
            if (copies[j].mask->rotationDeg == m->rotationDeg &&
                copies[j-1].mask->rotationDeg == last.mask->rotationDeg)
              propose(last.x + copies[j].x - copies[j-1].x,
                      last.y + copies[j].y - copies[j-1].y, r);
          }
        }
        // Duplicates only occur within one rotation. Sort a small contiguous
        // range instead of allocating a hash-table node for every proposal.
        auto firstIt=localCandidates.begin()+first;
        auto positionLess=[](const Candidate& a,const Candidate& b) { return std::tie(a.x,a.y)<std::tie(b.x,b.y); };
        std::sort(firstIt,localCandidates.end(),positionLess);
        localCandidates.erase(std::unique(firstIt,localCandidates.end(),[](const Candidate& a,const Candidate& b) {
          return a.x==b.x && a.y==b.y;
        }),localCandidates.end());
      }
      });
      size_t count=0;
      for(const auto& batch:workerCandidates) count+=batch.size();
      candidates.reserve(count);
      for(size_t slot=0;slot<parallel.size();++slot) {
        candidates.insert(candidates.end(),workerCandidates[slot].begin(),workerCandidates[slot].end());
        localStats.rejectedPositionSkips+=workerSkipped[slot];
      }
      // Tiles may propose the same contact. Remove duplicates before the
      // ranked shortlist so its contents do not depend on the thread count.
      std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b) {
        return std::tie(a.rotation,a.x,a.y)<std::tie(b.rotation,b.x,b.y);
      });
      candidates.erase(std::unique(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b) {
        return a.rotation==b.rotation && a.x==b.x && a.y==b.y;
      }),candidates.end());
    }
    if (!fromPattern && !exhausted.contains(searchIdentity)) {
      if(config.bitmapBottomLeft && seeds.empty()) {
        PhaseTimer timer{localStats.phases.searchMs};
        std::sort(candidates.begin(),candidates.end(),[&](const Candidate& a,const Candidate& b) {
          if(config.bitmapNfpContacts && config.bitmapContactColumnFirst)
            return std::tie(a.x,a.y,a.rotation)<std::tie(b.x,b.y,b.rotation);
          return std::tie(a.y,a.x,a.rotation)<std::tie(b.y,b.x,b.rotation);
        });
        evaluateOrdered(candidates,true);
        // No grid fallback: a failed shortlist does not prove that the shape
        // cannot fit. Later placements can create new contact candidates.
      } else {
      auto rank = [](const Candidate& a, const Candidate& b) {
        return std::tie(a.area,a.extent,a.x,a.y,a.rotation) < std::tie(b.area,b.extent,b.x,b.y,b.rotation);
      };

      // Start with the occupied rectangle plus one part, growing only when necessary.
      {
      PhaseTimer timer{localStats.phases.searchMs};
      int windowW = std::min(material.widthPx, usedWidth + maxWidth);
      int windowH = std::min(material.heightPx, usedHeight + maxHeight);
      while (true) {
        deadline.check();
        std::vector<Candidate> inWindow;
        inWindow.reserve(candidates.size());
        for (const auto& c : candidates) {
          const auto* m = rotationMasks[c.rotation];
          if (c.x + m->widthPx > windowW || c.y + m->heightPx > windowH) continue;
          if (!rejected.contains(rejected.index(c.x,c.y,c.rotation))) inWindow.push_back(c);
        }
        const auto limit = std::min<size_t>(8192, inWindow.size());
        if(limit<inWindow.size()) std::nth_element(inWindow.begin(),inWindow.begin()+limit,inWindow.end(),rank);
        std::sort(inWindow.begin(),inWindow.begin()+limit,rank);
        evaluateOrdered(std::span<const Candidate>(inWindow).first(limit),true);
        if (!bestScore) {
          // GPU and CPU share the same deterministic grid order and validation.
          const int coarse = std::max(searchStep, static_cast<int>(std::ceil(
              std::sqrt(static_cast<double>(windowW) * windowH / 1024.0))));
          const uint64_t rows=(uint64_t(windowH)+coarse-1)/coarse;
          const uint64_t end=((uint64_t(windowW)+coarse-1)/coarse)*rows*rotationMasks.size();
          for(uint64_t first=0;first<end;) {
            deadline.check();
            const uint32_t count=uint32_t(std::min<uint64_t>(config.gpuBatchSize,end-first));
            const auto flags=gpuGrid(first,count,uint32_t(rows),coarse,windowW,windowH);
            for(size_t offset=0;offset<count;offset+=validationBatch) {
              const size_t n=std::min(validationBatch,size_t(count)-offset);
              std::vector<Candidate> batch; batch.reserve(n);
              std::vector<uint8_t> allowed; allowed.reserve(n);
              for(size_t i=0;i<n;++i) {
                const uint64_t p=first+offset+i;
                const size_t r=size_t(p%rotationMasks.size());
                const int x=int(p/rotationMasks.size()/rows)*coarse;
                const int y=int((p/rotationMasks.size())%rows)*coarse;
                if(x+rotationMasks[r]->widthPx>windowW || y+rotationMasks[r]->heightPx>windowH) continue;
                batch.push_back({x,y,r});
                allowed.push_back(!flags || (*flags)[offset+i]);
              }
              evaluateBatch(batch,false,allowed,bool(flags),false);
            }
            first+=count;
          }
        }
        if (bestScore || (windowW == material.widthPx && windowH == material.heightPx)) break;
        ++localStats.searchWindowExpansions;
        windowW = std::min(material.widthPx, std::max(windowW + maxWidth, windowW * 2));
        windowH = std::min(material.heightPx, std::max(windowH + maxHeight, windowH * 2));
      }
      // Fine fallback preserves small feasible pockets missed by contact/coarse proposals.
      // It runs only after the fast search fails; identical exhausted shapes skip it later.
      if (!bestScore && !config.bitmapBottomLeft) {
        ++localStats.fineFallbacks;
        // Failed grid positions stay invalid as occupancy grows. Repeated parts resume
        // the fine scan rather than rechecking the same occupied prefix each time.
        if (globalMaxOriginX >= 0 && globalMaxOriginY >= 0) {
          const uint64_t rows = static_cast<uint64_t>(globalMaxOriginY / searchStep) + 1;
          const uint64_t columns = static_cast<uint64_t>(globalMaxOriginX / searchStep) + 1;
          const uint64_t rotations = rotationMasks.size();
          auto& cursor = fineSearchCursor[searchIdentity];
          const uint64_t end = rows * columns * rotations;
          while (!bestScore && cursor < end) {
            deadline.check();
            if(gpuAvailable) {
              const uint32_t count=uint32_t(std::min<uint64_t>(config.gpuBatchSize,end-cursor));
              const auto flags=gpuGrid(cursor,count,uint32_t(rows),searchStep,material.widthPx,material.heightPx);
              if(flags) {
                const uint64_t first=cursor;
                for(size_t offset=0;offset<count && !bestScore;offset+=validationBatch) {
                  const size_t n=std::min(validationBatch,size_t(count)-offset);
                  std::vector<Candidate> batch; batch.reserve(n);
                  for(size_t i=0;i<n;++i) {
                    const uint64_t p=first+offset+i;
                    batch.push_back({int(p/rotations/rows)*searchStep,int((p/rotations)%rows)*searchStep,size_t(p%rotations)});
                  }
                  const auto found=evaluateBatch(batch,true,std::span<const uint8_t>(*flags).subspan(offset,n),true,false);
                  cursor=first+offset+(found ? *found : n);
                }
                continue;
              }
            }
            std::vector<Candidate> batch;
            std::vector<uint64_t> positions;
            uint64_t next=cursor;
            while(next<end && batch.size()<validationBatch) {
              if(searchStep==1) {
                const auto available=rejected.next(next);
                localStats.rejectedPositionSkips+=size_t(available-next); next=available;
              }
              if(next>=end) break;
              positions.push_back(next);
              batch.push_back({int(next/rotations/rows)*searchStep,int((next/rotations)%rows)*searchStep,size_t(next%rotations)});
              ++next;
            }
            const auto found=evaluateBatch(batch,true,{},false,false);
            cursor=found ? positions[*found] : next;
          }
        }
      }
      // Coarse-to-fine local compaction, including all permitted rotations at each level.
      }
      if (bestScore) {
        PhaseTimer timer{localStats.phases.refinementMs};
        for (int step = std::max(searchStep, std::min(maxWidth, maxHeight) / 4);;
             step = std::max(searchStep, step / 2)) {
          for (int iteration = 0; iteration < 8; ++iteration) {
            const int startX = acceptedX, startY = acceptedY;
            const auto previousScore = *bestScore;
            std::vector<Candidate> batch;
            for (int dx : {-step, 0, step})
              for (int dy : {-step, 0, step})
                for (size_t r = 0; r < rotationMasks.size(); ++r) {
                  batch.push_back({startX+dx,startY+dy,r});
                  if(batch.size()==size_t(config.gpuBatchSize)) { evaluateOrdered(batch,false); batch.clear(); }
                }
            if(!batch.empty()) evaluateOrdered(batch,false);
            if (*bestScore == previousScore) break;
          }
          if (step == searchStep) break;
        }
      } else {
        if (!config.bitmapBottomLeft && searchStep == 1) exhausted.insert(searchIdentity);
      }
      }
    } else if (!fromPattern) {
      ++localStats.exhaustedShapeSkips;
    }
    }
    } catch(const SearchTimeExpired&) {
      // Keep the best fully validated candidate even if refinement was stopped.
      localStats.timeLimitReached=true;
    }

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - partStart).count();

    if (!acceptedPlacement.has_value() || acceptedMask == nullptr) {
      if(config.bitmapBottomLeft && config.bitmapCacheRejects && !localStats.timeLimitReached)
        failedContactVersions[searchIdentity]=placedCount;
      out.unplaced.push_back(part);
      ++unplacedCount;
      localStats.unplacedParts++;
      localStats.processedParts++;
      localStats.candidatesExamined += partCandidatesExamined;
      localStats.boundaryRejects += partBoundaryRejects;
      localStats.bitmapCollisions += partBitmapCollisions;
      localStats.vectorValidationRejects += partVectorValidationRejects;
      auto& partStats = localStats.perPart.emplace_back(BitmapNestingStats::PartStats{
          partIndex + 1,
          false,
          placedCount,
          unplacedCount,
          elapsedMs,
          partCandidatesExamined,
          partBoundaryRejects,
          partBitmapCollisions,
          partVectorValidationRejects});
      if (onPartProgress) {
        onPartProgress(partStats, totalParts);
      }
      continue;
    }

    if(!deadline.expired()) {
      pixels.ensure(*acceptedMask,false);
      applyMask(occupancy, *acceptedMask, acceptedX, acceptedY, useAvx2);
    }
    placements.push_back(*acceptedPlacement);
    occupyProposalSpace(acceptedX,acceptedY,acceptedMask->widthPx,acceptedMask->heightPx);
    RasterPlacement raster{acceptedX, acceptedY, acceptedMask, identity};
    auto core=acceptedMask->filledCore;
    core.x+=acceptedX;core.y+=acceptedY;
    // Thin cores of stars/rings provide little proof and fragment the free
    // rectangles severely. Omitting an obstacle only weakens this rejection.
    const int64_t maskArea=int64_t(acceptedMask->widthPx)*acceptedMask->heightPx;
    if(int64_t(core.width)*core.height>=maskArea/2) freeRectangles.occupy(core);
    auto exactCore=acceptedMask->geometricCore;
    if(exactCore.width>0 && exactCore.height>0 && int64_t(exactCore.width)*exactCore.height>=maskArea/2) {
      exactCore.x+=acceptedX;exactCore.y+=acceptedY;
      const double gap=std::min(config.spacing,config.holeSpacing)/config.bitmapResolutionMm;
      const int axis=int(std::min(double(std::max(material.widthPx,material.heightPx)),std::floor(gap)));
      const int diagonal=int(std::min(double(std::max(material.widthPx,material.heightPx)),std::floor(gap/std::sqrt(2.0))));
      // This cross plus its corner square lies inside the Euclidean clearance
      // offset. It cannot reject a legal diagonal placement or a nesting hole.
      clearanceRectangles.occupy({exactCore.x-axis,exactCore.y,exactCore.width+2*axis,exactCore.height});
      clearanceRectangles.occupy({exactCore.x,exactCore.y-axis,exactCore.width,exactCore.height+2*axis});
      clearanceRectangles.occupy({exactCore.x-diagonal,exactCore.y-diagonal,exactCore.width+2*diagonal,exactCore.height+2*diagonal});
    }
    rasterPlacements.push_back(raster);
    repeated[identity].push_back(raster);
    usedWidth = std::max(usedWidth, acceptedX + acceptedMask->widthPx);
    usedHeight = std::max(usedHeight, acceptedY + acceptedMask->heightPx);
    Polygon placedPolygon;
    if (config.bitmapValidateGeometry) {
      placedPolygon = acceptedAbsolute;
      placedBounds.push_back(acceptedBounds);
    } else {
      placedPolygon = shiftPolygon(acceptedMask->rotatedPart, {acceptedPlacement->x, acceptedPlacement->y, true});
      placedBounds.push_back(Bounds{acceptedMask->rotatedBounds.x + acceptedPlacement->x,
                                    acceptedMask->rotatedBounds.y + acceptedPlacement->y,
                                    acceptedMask->rotatedBounds.width,
                                    acceptedMask->rotatedBounds.height});
    }
    neighbours.insert(placedBounds.back(), placedAbsolute.size());
    placedAbsolute.push_back(placedPolygon);
    if(config.bitmapPatternTrial)
      for(const auto& hole:placedPolygon.children)
        holePockets.push_back({hole,getPolygonBounds(hole.points)});
    out.area += polygonMaterialArea(placedPolygon);
    ++placedCount;
    localStats.acceptedPlacements++;
    localStats.placedParts++;
    localStats.processedParts++;
    localStats.candidatesExamined += partCandidatesExamined;
    localStats.boundaryRejects += partBoundaryRejects;
    localStats.bitmapCollisions += partBitmapCollisions;
    localStats.vectorValidationRejects += partVectorValidationRejects;
    auto& partStats = localStats.perPart.emplace_back(BitmapNestingStats::PartStats{
        partIndex + 1,
        true,
        placedCount,
        unplacedCount,
        elapsedMs,
        partCandidatesExamined,
        partBoundaryRejects,
        partBitmapCollisions,
        partVectorValidationRejects});
    if (onPartProgress) {
      onPartProgress(partStats, totalParts);
    }
  }

  localStats.cachedMaskCount = maskCache.size();
  localStats.occupiedBoundsArea = double(usedWidth) * usedHeight * config.bitmapResolutionMm * config.bitmapResolutionMm;
  localStats.simdBackend = useAvx2 ? "avx2" : "scalar";
  localStats.totalBitmapMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bitmapStart).count();
  if (stats != nullptr) {
    *stats = std::move(localStats);
  }
  if (!placements.empty()) {
    out.placements.push_back(SheetPlacement{sheet.source, sheet.id, placements});
  }
  out.utilisation = out.totalarea > 0.0 ? (out.area / out.totalarea) * 100.0 : 0.0;
  out.fitness = static_cast<double>(out.unplaced.size());
  return out;
}

#include "recursive_bitmap_search.hpp"

}  // namespace

// CPU companion search: compact a copy of the incumbent, then try refilling.
// It never mutates a live recursive tree or its GPU occupancy.
PlacementResult refillBitmapLayout(const std::vector<Polygon>& sheets,
    const std::vector<Polygon>& parts,const PlacementResult& incumbent,
    const Config& config,BitmapNestingStats* stats) {
  const auto started=std::chrono::steady_clock::now();
  const SearchDeadline deadline(config.timeLimitSeconds,config.stopRequested);
  auto result=incumbent;BitmapNestingStats counts;
  auto cfg=config;cfg.gpuEnabled=false;cfg.threads=1;cfg.bitmapPatternTrial=false;
  cfg.bitmapBottomLeft=true;cfg.mode=SearchMode::First;
  // On later passes release a small spatial neighbourhood, including early
  // placements that depth-first backtracking may not revisit for hours.
  if(cfg.searchIteration>0 && !result.placements.empty()) {
    auto& layout=result.placements[(cfg.searchIteration-1)%result.placements.size()];
    if(!layout.sheetplacements.empty()) {
      const auto anchor=layout.sheetplacements[(cfg.searchIteration-1)%layout.sheetplacements.size()];
      std::vector<size_t> order(layout.sheetplacements.size());std::iota(order.begin(),order.end(),0);
      std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
        const auto& x=layout.sheetplacements[a];const auto& y=layout.sheetplacements[b];
        return std::hypot(x.x-anchor.x,x.y-anchor.y)<std::hypot(y.x-anchor.x,y.y-anchor.y);
      });
      const size_t count=std::min<size_t>(3+(cfg.searchIteration/7)%6,order.size());
      std::unordered_set<size_t> removed(order.begin(),order.begin()+count);
      std::vector<Placement> kept;
      for(size_t i=0;i<layout.sheetplacements.size();++i) {
        const auto& placement=layout.sheetplacements[i];
        if(!removed.contains(i)) {kept.push_back(placement);continue;}
        const auto original=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.id==placement.id;});
        if(original==parts.end()) throw std::invalid_argument("Unknown removed part in refill incumbent");
        // Put released parts first so the repair restores its neighbourhood.
        result.unplaced.insert(result.unplaced.begin(),*original);
      }
      layout.sheetplacements=std::move(kept);
    }
  }
  refineVectors(sheets,parts,result,cfg,deadline,counts);
  ParallelLoop parallel(1);
  detail::RejectionCacheBudget rejectionBudget(16*1024*1024);
  std::unique_ptr<GpuBitmap> gpu;
  // Cycle a small angle shortlist; repeated passes cover the input angle policy.
  auto remaining=result.unplaced;
  for(auto& part:remaining) {
    const auto allowed=part.allowedAngles;
    if(allowed.size()>12) {
      part.allowedAngles.clear();
      for(size_t i=0;i<12;++i)
        part.allowedAngles.push_back(allowed[(i*allowed.size()/12+cfg.searchIteration)%allowed.size()]);
      // Include the released copy's previous orientation, otherwise narrow
      // pockets could never be restored by the coarse rotation shortlist.
      for(const auto& layout:incumbent.placements) for(const auto& placement:layout.sheetplacements)
        if(placement.id==part.id && std::find(part.allowedAngles.begin(),part.allowedAngles.end(),placement.rotation)==part.allowedAngles.end())
          part.allowedAngles.insert(part.allowedAngles.begin(),placement.rotation);
    }
  }
  for(const auto& sheet:sheets) {
    if(remaining.empty() || deadline.expired()) break;
    auto layout=std::find_if(result.placements.begin(),result.placements.end(),[&](const auto& p){return p.sheetid==sheet.id;});
    // Search existing sheets only; a companion must not consume extra stock.
    if(layout==result.placements.end()) continue;
    std::vector<std::pair<Placement,Polygon>> seeds;
    for(const auto& placement:layout->sheetplacements) {
      const auto original=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.id==placement.id;});
      if(original==parts.end()) throw std::invalid_argument("Unknown part in refill incumbent");
      seeds.emplace_back(placement,shiftPolygon(rotatePolygon(*original,placement.rotation),{placement.x,placement.y,true}));
    }
    BitmapNestingStats fillStats;
    auto filled=placePartsBitmapOnSingleSheet(sheet,remaining,cfg,parallel,rejectionBudget,gpu,deadline,false,&fillStats,{},seeds);
    counts.vectorChecks+=fillStats.vectorChecks;counts.candidatesExamined+=fillStats.candidatesExamined;
    if(filled.placements.empty() || filled.placements.front().sheetplacements.size()<=seeds.size()) continue;
    counts.refillPlacements+=filled.placements.front().sheetplacements.size()-seeds.size();++counts.refillPasses;
    layout->sheetplacements=std::move(filled.placements.front().sheetplacements);
    remaining=std::move(filled.unplaced);
  }
  // Restore original complete angle policies for every still-unplaced copy.
  result.unplaced.clear();result.area=0;result.totalarea=0;counts.occupiedBoundsArea=0;
  std::unordered_set<int> placed;
  for(const auto& layout:result.placements) {
    const auto sheet=std::find_if(sheets.begin(),sheets.end(),[&](const auto& p){return p.id==layout.sheetid;});
    const auto sb=getPolygonBounds(sheet->points);double right=0,top=0,left=sb.width,bottom=sb.height;
    if(!layout.sheetplacements.empty()) result.totalarea+=polygonMaterialArea(*sheet);
    for(const auto& placement:layout.sheetplacements) {
      placed.insert(placement.id.value());
      const auto source=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.id==placement.id;});
      const auto geometry=shiftPolygon(rotatePolygon(*source,placement.rotation),{placement.x,placement.y,true});
      const auto b=getPolygonBounds(geometry.points);right=std::max(right,b.x+b.width-sb.x);top=std::max(top,b.y+b.height-sb.y);
      left=std::min(left,b.x-sb.x);bottom=std::min(bottom,b.y-sb.y);
    }
    if(!layout.sheetplacements.empty()) counts.occupiedBoundsArea+=(right-left)*(top-bottom);
  }
  for(const auto& part:parts) {
    if(placed.contains(part.id.value())) result.area+=polygonMaterialArea(part);
    else result.unplaced.push_back(part);
  }
  result.fitness=double(result.unplaced.size());result.utilisation=result.totalarea?100*result.area/result.totalarea:0;
  counts.placedParts=counts.acceptedPlacements=placed.size();counts.unplacedParts=result.unplaced.size();
  counts.cancelled=deadline.cancelled();counts.timeLimitReached=deadline.timedOut();
  counts.totalBitmapMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  if(stats) *stats=counts;
  return result;
}

// Report whether this build and CPU can use AVX2 bitmap operations.
bool bitmapAvx2Supported() {
#if defined(__AVX2__)
  return avx2RuntimeAvailable();
#else
  return false;
#endif
}

// Run bitmap strategies and retain the best validated layout.
PlacementResult placePartsBitmap(const std::vector<Polygon>& sheets,
                                 const std::vector<Polygon>& parts,
                                 const Config& config,
                                 BitmapNestingStats* stats,
                                 const BitmapPartProgressCallback& onPartProgress,
                                 const BitmapLayoutCallback& onLayout) {
  if(config.gpuBatchSize<256 || config.gpuBatchSize>262144 || config.gpuDevice < -1)
    throw std::invalid_argument("Invalid GPU configuration");
  for(double gap:{config.spacing,config.sheetSpacing,config.holeSpacing})
    if(!std::isfinite(gap) || gap<0 || gap>1000000)
      throw std::invalid_argument("Clearances must be finite numbers between 0 and 1000000 mm");
  if (!std::isfinite(config.bitmapResolutionMm) || config.bitmapResolutionMm <= 0.0) {
    throw std::invalid_argument("--bitmap-resolution must be a positive number");
  }
  if (config.bitmapSearchStepPx <= 0) {
    throw std::invalid_argument("--bitmap-step must be a positive integer");
  }
  for(const auto& part:parts) {
    if(part.allowedAngles.empty())
      throw std::invalid_argument("Every part requires at least one permitted orientation");
    if(part.allowedAngles.size()>Config::maxRotations)
      throw std::invalid_argument("At most 3600 allowedAngles per part are supported");
    for(double angle:part.allowedAngles)
      if(!std::isfinite(angle) || angle<0 || angle>=360)
        throw std::invalid_argument("Library allowedAngles must be normalized to [0,360)");
  }
  if(config.mode==SearchMode::Continuous) return parallelRecursiveSearch(sheets,parts,config,stats,onLayout);
  const SearchDeadline deadline(config.timeLimitSeconds,config.stopRequested);
  if (sheets.empty()) {
    return {};
  }

  const bool useAvx2 =
#if defined(__AVX2__)
      config.bitmapPreferAvx2 && avx2RuntimeAvailable();
#else
      false;
#endif

  const auto start=std::chrono::steady_clock::now();
  const bool hasHoles=std::any_of(parts.begin(),parts.end(),[](const Polygon& p) { return !p.children.empty(); });
  const int trialCount=config.bitmapBottomLeft || (parts.size()<6 && !hasHoles) ? 1 : std::clamp(config.bitmapTrials,1,4);
  if(config.bitmapRejectCacheMiB<1||config.bitmapRejectCacheMiB>4096)
    throw std::invalid_argument("cacheMemoryMiB must be between 1 and 4096");
  detail::RejectionCacheBudget rejectionBudget(size_t(config.bitmapRejectCacheMiB)*1024*1024);
  const int workerCount=std::min(trialCount,normalizeWorkerCount(config.threads));
  const int helpersPerTrial=std::max(1,normalizeWorkerCount(config.threads)/workerCount);
  // Keep one independently executed strategy's result and diagnostics.
  struct Trial { PlacementResult result; BitmapNestingStats stats; double elapsedMs{0}; bool started{false}; };
  std::vector<Trial> trials(size_t(trialCount), Trial{});
  std::atomic<int> next{0};
  std::exception_ptr error;
  std::mutex errorMutex;
  std::mutex layoutMutex;
  auto work=[&] {
    try {
      ParallelLoop parallel{size_t(helpersPerTrial)};
      for (;;) {
        if(deadline.expired()) break;
        const int i=next.fetch_add(1);
        if (i>=trialCount) break;
        const auto trialStart=std::chrono::steady_clock::now();
        auto cfg=config;
        // First is a single large-first contact layout. Timed iterations also
        // compare a fast contact strategy against the existing broader search.
        cfg.bitmapBottomLeft=config.bitmapBottomLeft || (config.mode!=SearchMode::First && i==0 && config.timeLimitSeconds>0);
        cfg.bitmapPatternTrial=i==1 || config.mode==SearchMode::First || cfg.bitmapBottomLeft;
        auto remaining=parts;
        if(config.searchIteration>0) {
          std::mt19937_64 random(config.searchIteration*0x9e3779b97f4a7c15ULL+uint64_t(i));
          std::shuffle(remaining.begin(),remaining.end(),random);
          // Cycle through the angle list instead of randomly revisiting the
          // same preferred angles. Input-order perturbations remain seeded.
          cfg.searchIteration=(config.searchIteration-1)*uint64_t(trialCount)+uint64_t(i)+1;
        } else if (i>=2) std::stable_sort(remaining.begin(),remaining.end(),[&](const Polygon& a,const Polygon& b) {
          return i==2 ? polygonMaterialArea(a)>polygonMaterialArea(b) : polygonMaterialArea(a)<polygonMaterialArea(b);
        });
        if(cfg.bitmapBottomLeft && (config.mode==SearchMode::First || config.searchIteration==0)) {
          std::stable_sort(remaining.begin(),remaining.end(),[](const Polygon& a,const Polygon& b) {
            return polygonMaterialArea(a)>polygonMaterialArea(b);
          });
        } else if(cfg.bitmapPatternTrial && config.searchIteration==0) {
          auto largestHole=[](const Polygon& p) {
            double area=0;
            for(const auto& hole:p.children) area=std::max(area,std::abs(polygonArea(hole)));
            return area;
          };
          // Large cavities first, then large inserts before small ones. Timed
          // search retains independent input/large/small-order alternatives.
          std::stable_sort(remaining.begin(),remaining.end(),[&](const Polygon& a,const Polygon& b) {
            const double ah=largestHole(a),bh=largestHole(b);
            if(ah!=bh) return ah>bh;
            return polygonMaterialArea(a)>polygonMaterialArea(b);
          });
        }
        auto& trial=trials[size_t(i)];
        trial.started=true;
        trial.stats.vectorRefinementCompleted=true;
        // A strategy runs its sheets serially. Reuse its OpenCL context and
        // compiled kernels while replacing sheet/mask/occupancy buffers.
        std::unique_ptr<GpuBitmap> gpu;
        for (const auto& sheet:sheets) {
          if (remaining.empty()) break;
          if(deadline.expired()) { trial.stats.timeLimitReached=true; break; }
          BitmapNestingStats s;
          auto r=placePartsBitmapOnSingleSheet(sheet,remaining,cfg,parallel,rejectionBudget,gpu,deadline,useAvx2,&s,{});
          // Finish each sheet before consuming fresh stock. Vector compaction
          // can open a whole new row; rebuild and refill until no copy is added.
          // Every successful pass reduces the finite remaining-part count.
          refineVectors({sheet},parts,r,cfg,deadline,s);
          while(!r.unplaced.empty() && !r.placements.empty() && !deadline.expired()) {
            std::vector<std::pair<Placement,Polygon>> seeds;
            for(const auto& p:r.placements.front().sheetplacements) {
              const auto source=std::find_if(parts.begin(),parts.end(),[&](const Polygon& shape){return shape.id==p.id;});
              seeds.emplace_back(p,shiftPolygon(rotatePolygon(*source,p.rotation),{p.x,p.y,true}));
            }
            auto fillConfig=cfg;
            fillConfig.bitmapPatternTrial=false;
            BitmapNestingStats fillStats;
            auto filled=placePartsBitmapOnSingleSheet(sheet,r.unplaced,fillConfig,parallel,rejectionBudget,gpu,deadline,useAvx2,&fillStats,{},seeds);
            s.timeLimitReached=s.timeLimitReached || fillStats.timeLimitReached;
            s.phases+=fillStats.phases;
            s.candidatesExamined+=fillStats.candidatesExamined;
            s.acceptedPlacements+=fillStats.acceptedPlacements;
            s.boundaryRejects+=fillStats.boundaryRejects;
            s.bitmapCollisions+=fillStats.bitmapCollisions;
            s.vectorValidationRejects+=fillStats.vectorValidationRejects;
            s.neighbourGeometryChecks+=fillStats.neighbourGeometryChecks;
            s.searchWindowExpansions+=fillStats.searchWindowExpansions;
            s.exhaustedShapeSkips+=fillStats.exhaustedShapeSkips;
            s.rejectedPositionSkips+=fillStats.rejectedPositionSkips;
            s.cachedMaskCount+=fillStats.cachedMaskCount;
            if(!fillStats.gpuDevice.empty()) s.gpuDevice=fillStats.gpuDevice;
            if(!fillStats.gpuFallbackReason.empty()) s.gpuFallbackReason=fillStats.gpuFallbackReason;
            s.fineFallbacks+=fillStats.fineFallbacks;
            s.gpuCandidates+=fillStats.gpuCandidates;s.gpuBatches+=fillStats.gpuBatches;
            s.candidateWorkersUsed=std::max(s.candidateWorkersUsed,fillStats.candidateWorkersUsed);
            // An interrupted rebuild may contain only a prefix of the seeds.
            const size_t filledCount=filled.placements.empty()?0:filled.placements.front().sheetplacements.size();
            if(filledCount<=seeds.size()) break;
            s.refillPlacements+=filledCount-seeds.size();
            ++s.refillPasses;
            r=std::move(filled);
            refineVectors({sheet},parts,r,cfg,deadline,s);
          }
          trial.result.area+=r.area;
          trial.result.totalarea+=r.totalarea;
          trial.result.placements.insert(trial.result.placements.end(),r.placements.begin(),r.placements.end());
          remaining=std::move(r.unplaced);
          auto& t=trial.stats;
          t.timeLimitReached=t.timeLimitReached || s.timeLimitReached;
          if(!s.gpuDevice.empty()) t.gpuDevice=s.gpuDevice;
          if(!s.gpuFallbackReason.empty()) t.gpuFallbackReason=s.gpuFallbackReason;
          t.gpuCandidates+=s.gpuCandidates;
          t.gpuBatches+=s.gpuBatches;
          t.cachedMaskCount+=s.cachedMaskCount;
          t.phases+=s.phases;
          t.acceptedPlacements+=s.acceptedPlacements;
          t.candidatesExamined+=s.candidatesExamined;
          t.boundaryRejects+=s.boundaryRejects;
          t.bitmapCollisions+=s.bitmapCollisions;
          t.vectorValidationRejects+=s.vectorValidationRejects;
          t.neighbourGeometryChecks+=s.neighbourGeometryChecks;
          t.searchWindowExpansions+=s.searchWindowExpansions;
          t.fineFallbacks+=s.fineFallbacks;
          t.exhaustedShapeSkips+=s.exhaustedShapeSkips;
          t.failedSearchSkips+=s.failedSearchSkips;
          t.rejectedPositionSkips+=s.rejectedPositionSkips;
          t.patternPlacements+=s.patternPlacements;
          t.maxPatternGroupSize=std::max(t.maxPatternGroupSize,s.maxPatternGroupSize);
          t.maxInterlockingCapacity=std::max(t.maxInterlockingCapacity,s.maxInterlockingCapacity);
          t.holePlacements+=s.holePlacements;
          t.refillPlacements+=s.refillPlacements;t.refillPasses+=s.refillPasses;
          t.vectorMoves+=s.vectorMoves;t.vectorChecks+=s.vectorChecks;
          t.vectorRefinementMs+=s.vectorRefinementMs;
          t.vectorRefinementCompleted=t.vectorRefinementCompleted && s.vectorRefinementCompleted;
          t.candidateWorkersUsed=std::max(t.candidateWorkersUsed,s.candidateWorkersUsed);
          t.occupiedBoundsArea+=s.occupiedBoundsArea;
          t.perPart.insert(t.perPart.end(),s.perPart.begin(),s.perPart.end());
        }
        trial.result.unplaced=std::move(remaining);
        trial.result.fitness=double(trial.result.unplaced.size());
        trial.result.utilisation=trial.result.totalarea>0 ? 100*trial.result.area/trial.result.totalarea : 0;
        trial.stats.processedParts=parts.size();
        trial.stats.unplacedParts=trial.result.unplaced.size();
        trial.stats.placedParts=parts.size()-trial.result.unplaced.size();
        trial.elapsedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-trialStart).count();
        if(onLayout) {
          auto snapshot=trial.stats;
          snapshot.cancelled=deadline.cancelled();
          snapshot.timeLimitReached=snapshot.timeLimitReached && deadline.timedOut();
          snapshot.totalBitmapMs=trial.elapsedMs;
          snapshot.selectedTrial=size_t(i);
          snapshot.startedTrials=1;
          snapshot.completedTrials=trial.stats.timeLimitReached ? 0 : 1;
          snapshot.workersUsed=workerCount;
          snapshot.proposalWorkersPerTrial=helpersPerTrial;
          snapshot.cpuWorkersUsed=workerCount*helpersPerTrial;
          snapshot.rejectionCachePeakBytes=rejectionBudget.peak();
          std::lock_guard lock(layoutMutex);
          onLayout(trial.result,snapshot);
        }
      }
    } catch (...) { std::lock_guard lock(errorMutex); if (!error) error=std::current_exception(); }
  };
  {
    std::vector<std::jthread> workers;
    for (int w=1; w<workerCount; ++w) workers.emplace_back(work);
    work();
  }
  if (error) std::rethrow_exception(error);
  auto scoreTrial=[](const Trial& t) {
    return std::tuple{t.result.unplaced.size(),t.result.totalarea-t.result.area,t.stats.occupiedBoundsArea};
  };
  size_t best=0;
  bool haveBest=false;
  for(size_t i=0; i<trials.size(); ++i) if(trials[i].started && (!haveBest || scoreTrial(trials[i])<scoreTrial(trials[best]))) {
    best=i; haveBest=true;
  }
  if(!haveBest) {
    trials[0].result.unplaced=parts;
    trials[0].result.fitness=double(parts.size());
    trials[0].stats.unplacedParts=parts.size();
  }
  auto& winner=trials[best];
  // GPU diagnostics describe all executed strategies, even when the winning
  // strategy found its layout without needing a GPU search batch.
  size_t gpuCandidates=0,gpuBatches=0;
  std::string gpuDevice,gpuFallbackReason;
  for(const auto& t:trials) {
    gpuCandidates+=t.stats.gpuCandidates; gpuBatches+=t.stats.gpuBatches;
    if(!t.stats.gpuDevice.empty()) gpuDevice=t.stats.gpuDevice;
    if(!t.stats.gpuFallbackReason.empty()) gpuFallbackReason=t.stats.gpuFallbackReason;
  }
  winner.stats.gpuCandidates=gpuCandidates; winner.stats.gpuBatches=gpuBatches;
  winner.stats.gpuDevice=std::move(gpuDevice); winner.stats.gpuFallbackReason=std::move(gpuFallbackReason);
  const char* strategyNames[]={config.bitmapBottomLeft ? "first_bottom_left" : config.timeLimitSeconds>0 ? "bottom_left" : "compact","holes_first_rows","large_first","small_first"};
  winner.stats.selectedTrial=best;
  size_t startedTrials=0,completedTrials=0;
  for(size_t i=0;i<trials.size();++i) if(trials[i].started) {
    ++startedTrials;
    if(!trials[i].stats.timeLimitReached) ++completedTrials;
    winner.stats.trials.push_back({strategyNames[i],trials[i].stats.placedParts,
        trials[i].stats.patternPlacements,trials[i].elapsedMs,trials[i].stats.phases,
        !trials[i].stats.timeLimitReached});
  }
  winner.stats.startedTrials=startedTrials;
  winner.stats.totalStartedTrials=startedTrials;
  winner.stats.completedTrials=completedTrials;
  winner.stats.cancelled=deadline.cancelled();
  winner.stats.timeLimitReached=completedTrials<size_t(trialCount) && deadline.timedOut();
  winner.stats.workersUsed=workerCount;
  winner.stats.proposalWorkersPerTrial=helpersPerTrial;
  winner.stats.cpuWorkersUsed=workerCount*helpersPerTrial;
  winner.stats.rejectionCachePeakBytes=rejectionBudget.peak();
  winner.stats.simdBackend=useAvx2 ? "avx2" : "scalar";
  winner.stats.totalBitmapMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  // Only publish the selected result, on the caller thread. Worker callbacks
  // never race each other or expose placements from a discarded strategy.
  if(onPartProgress) for(const auto& p:winner.stats.perPart) onPartProgress(p,winner.stats.perPart.size());
  if(stats) *stats=winner.stats;
  return std::move(winner.result);
}

}  // namespace clinesting
