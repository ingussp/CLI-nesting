#include "deepnestcpp/bitmap_nesting.hpp"

#include "deepnestcpp/geometry.hpp"
#include "deepnestcpp/nfp.hpp"
#include "deepnestcpp/gpu_bitmap.hpp"
#include "parallel_loop.hpp"
#include "search_deadline.hpp"
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

namespace deepnest {

namespace {

constexpr size_t kMaxBitmapPixels = 200000000ULL;

struct PhaseTimer {
  double& total;
  std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
  ~PhaseTimer() { total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); }
};

int rasterDimension(double mm, double resolution) {
  const double pixels=std::ceil(mm/resolution);
  if (!std::isfinite(pixels) || pixels>double(kMaxBitmapPixels))
    throw std::invalid_argument("Raster is too large; increase --bitmap-resolution");
  return std::max(1,static_cast<int>(pixels));
}

struct BitmapGrid {
  int widthPx{0};
  int heightPx{0};
  size_t wordsPerRow{0};
  std::vector<uint64_t> bits;
};

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
};

// Uniform spatial bins only select neighbours. Original polygons remain authoritative.
class SpatialIndex {
 public:
  explicit SpatialIndex(double cellSize) : cellSize_(cellSize) {}
  void insert(const Bounds& b, size_t id) {
    visit(b, [&](uint64_t key) { cells_[key].push_back(id); });
  }
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

struct RasterPlacement {
  int x, y;
  const RasterMask* mask;
  std::string identity;
};

struct Candidate {
  int x, y;
  size_t rotation;
  int64_t area{0};
  int extent{0};
};

// Failed origins remain invalid while occupancy only grows. Each strategy/sheet
// owns its cache; no bitmap or cached failure is shared between worker threads.
struct RejectedOrigins {
  uint64_t rows{0}, rotations{0}, size{0};
  std::vector<uint64_t> bits;
  uint64_t index(int x, int y, size_t r) const {
    return (uint64_t(x) * rows + uint64_t(y)) * rotations + r;
  }
  bool contains(uint64_t i) const {
    return !bits.empty() && i < size && (bits[i / 64] & (uint64_t(1) << (i % 64)));
  }
  void insert(uint64_t i) {
    if (!bits.empty() && i < size) bits[i / 64] |= uint64_t(1) << (i % 64);
  }
  uint64_t next(uint64_t i) const {
    if (bits.empty()) return i;
    while (i < size) {
      const uint64_t available = ~bits[i / 64] & (~uint64_t(0) << (i % 64));
      if (available) return std::min(size, (i / 64) * 64 + std::countr_zero(available));
      i = (i / 64 + 1) * 64;
    }
    return size;
  }
};

bool boundsIntersect(const Bounds& a, const Bounds& b) {
  const double aRight = a.x + a.width;
  const double aBottom = a.y + a.height;
  const double bRight = b.x + b.width;
  const double bBottom = b.y + b.height;
  return !(aRight <= b.x || bRight <= a.x || aBottom <= b.y || bBottom <= a.y);
}

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

void ensureBitmapFitsMemory(int widthPx, int heightPx, const char* what) {
  if (widthPx <= 0 || heightPx <= 0) {
    throw std::invalid_argument(std::string(what) + " raster dimensions must be positive");
  }

  const size_t pixels = static_cast<size_t>(widthPx) * static_cast<size_t>(heightPx);
  if (pixels > kMaxBitmapPixels) {
    throw std::invalid_argument(std::string(what) + " raster is too large; increase --bitmap-resolution");
  }
}

BitmapGrid makeBitmapGrid(int widthPx, int heightPx) {
  ensureBitmapFitsMemory(widthPx, heightPx, "Bitmap");
  BitmapGrid grid;
  grid.widthPx = widthPx;
  grid.heightPx = heightPx;
  grid.wordsPerRow = static_cast<size_t>((widthPx + 63) / 64);
  grid.bits.assign(grid.wordsPerRow * static_cast<size_t>(heightPx), 0ULL);
  return grid;
}

inline uint64_t* rowBits(BitmapGrid& grid, int y) {
  return &grid.bits[static_cast<size_t>(y) * grid.wordsPerRow];
}

inline const uint64_t* rowBits(const BitmapGrid& grid, int y) {
  return &grid.bits[static_cast<size_t>(y) * grid.wordsPerRow];
}

void setPixel(BitmapGrid& grid, int x, int y) {
  if (x < 0 || y < 0 || x >= grid.widthPx || y >= grid.heightPx) {
    return;
  }
  const size_t idx = static_cast<size_t>(y) * grid.wordsPerRow + static_cast<size_t>(x / 64);
  grid.bits[idx] |= (1ULL << static_cast<unsigned>(x % 64));
}

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

void rowsOrScalar(uint64_t* occ, const uint64_t* mask, size_t words) {
  for (size_t i = 0; i < words; ++i) {
    occ[i] |= mask[i];
  }
}

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
void rasterizeMaskPixels(const RasterMask& mask, double resolutionMm, const SearchDeadline* deadline) {
  mask.bits.assign(mask.wordsPerRow * static_cast<size_t>(mask.heightPx), 0ULL);

  for (int y = 0; y < mask.heightPx; ++y) {
    const double py = mask.minY + (static_cast<double>(y) + 0.5) * resolutionMm;
    for (int x = 0; x < mask.widthPx; ++x) {
      const double px = mask.minX + (static_cast<double>(x) + 0.5) * resolutionMm;
      if (pointInPolygonMaterial(mask.rotatedPart, {px, py, true})) {
        const size_t idx = static_cast<size_t>(y) * mask.wordsPerRow + static_cast<size_t>(x / 64);
        mask.bits[idx] |= (1ULL << static_cast<unsigned>(x % 64));
      }
    }
  }

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
}

// Pixel data can be discarded without invalidating placements, which retain
// stable pointers to the mask's geometry. Rebuild it on demand after eviction.
class RasterPixels {
 public:
  explicit RasterPixels(double resolution,const SearchDeadline& deadline) : resolution_(resolution),deadline_(deadline) {}
  void ensure(const RasterMask& mask,bool cancellable=true) {
    if(cancellable) deadline_.check();
    if (!mask.bits.empty()) return;
    const size_t estimate = mask.wordsPerRow * size_t(mask.heightPx) * sizeof(uint64_t)
                          + size_t(mask.heightPx) * sizeof(int);
    constexpr size_t budget = 64ULL * 1024 * 1024;
    while (!resident_.empty() && (estimate > budget || bytes_ > budget - estimate)) {
      const auto* old = resident_.front();
      resident_.pop_front();
      bytes_ -= old->bits.capacity() * sizeof(uint64_t) + old->collisionRows.capacity() * sizeof(int);
      std::vector<uint64_t>().swap(old->bits);
      std::vector<int>().swap(old->collisionRows);
    }
    try { rasterizeMaskPixels(mask, resolution_,cancellable ? &deadline_ : nullptr); }
    catch(const SearchTimeExpired&) { mask.bits.clear(); mask.collisionRows.clear(); throw; }
    bytes_ += mask.bits.capacity() * sizeof(uint64_t) + mask.collisionRows.capacity() * sizeof(int);
    resident_.push_back(&mask);
  }
 private:
  double resolution_;
  const SearchDeadline& deadline_;
  size_t bytes_{0};
  std::deque<const RasterMask*> resident_;
};

BitmapGrid rasterizeSheetMaterial(const Polygon& sheet, double resolutionMm, Bounds& sheetBounds,const SearchDeadline& deadline) {
  sheetBounds = getPolygonBounds(sheet.points);
  const int widthPx = rasterDimension(sheetBounds.width,resolutionMm);
  const int heightPx = rasterDimension(sheetBounds.height,resolutionMm);
  BitmapGrid material = makeBitmapGrid(widthPx, heightPx);

  for (int y = 0; y < material.heightPx; ++y) {
    deadline.check();
    const double py = sheetBounds.y + (static_cast<double>(y) + 0.5) * resolutionMm;
    for (int x = 0; x < material.widthPx; ++x) {
      const double px = sheetBounds.x + (static_cast<double>(x) + 0.5) * resolutionMm;
      if (pointInPolygonMaterial(sheet, {px, py, true})) {
        setPixel(material, x, y);
      }
    }
  }

  return material;
}

uint64_t shiftedMaskWord(const uint64_t* src, size_t srcWords, size_t wordIndex, int bitShift) {
  const uint64_t lo = wordIndex < srcWords ? src[wordIndex] : 0ULL;
  const uint64_t hi = wordIndex > 0 && wordIndex - 1 < srcWords ? src[wordIndex - 1] : 0ULL;
  return (lo << bitShift) | (hi >> (64 - bitShift));
}

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

void rowsOrShiftedScalar(uint64_t* occ, const uint64_t* maskRow, size_t maskWords, int bitShift, size_t shiftedWords) {
  for (size_t i = 0; i < shiftedWords; ++i) {
    occ[i] |= shiftedMaskWord(maskRow, maskWords, i, bitShift);
  }
}

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

std::string bitmapMaskCacheKey(const Polygon& part, double rotation) {
  std::ostringstream key;
  key<<polygonGeometryIdentity(part)<<'|'<<std::setprecision(17)<<rotation;
  return key.str();
}

std::string searchPolicyKey(const Polygon& part) {
  std::ostringstream key;
  key<<bitmapMaskCacheKey(part,part.rotation);
  if(!part.allowedAngles.empty()) {
    key<<"|allowed:"<<std::setprecision(17);
    for(double angle:part.allowedAngles) key<<angle<<';';
  }
  return key.str();
}

struct PairPattern {
  Candidate a{0,0,0}, b{0,0,0};
  int width{0}, height{0}, columns{0}, rows{0}, members{1};
  uint64_t cursor{0};
  uint64_t capacity() const { return uint64_t(columns) * rows * members; }
  Candidate next() {
    const auto index = cursor++;
    const auto cell = index / members;
    auto c = members == 2 && index % 2 ? b : a;
    c.x += int(cell % columns) * width;
    c.y += int(cell / columns) * height;
    return c;
  }
};

// A cell contains one part or a validated interlocking pair. Cells themselves
// have disjoint bounding boxes, so even nonadjacent copies cannot intersect.
PairPattern makePairPattern(const std::vector<const RasterMask*>& masks, int sheetW, int sheetH,
                            const Config& config, NfpCache& cache, RasterPixels& pixels,const SearchDeadline& deadline) {
  PairPattern best;
  auto consider = [&](Candidate a, Candidate b, int members) {
    deadline.check();
    const auto& am = *masks[a.rotation];
    const auto& bm = *masks[b.rotation];
    const int minX = std::min(a.x,b.x), minY = std::min(a.y,b.y);
    a.x -= minX; b.x -= minX; a.y -= minY; b.y -= minY;
    const int w = std::max(a.x+am.widthPx, members==2 ? b.x+bm.widthPx : 0);
    const int h = std::max(a.y+am.heightPx, members==2 ? b.y+bm.heightPx : 0);
    if (w<=0 || h<=0 || w>sheetW || h>sheetH) return;
    PairPattern p{a,b,w,h,sheetW/w,sheetH/h,members};
    if (p.capacity() < best.capacity()) return;
    if (p.capacity() == best.capacity() && best.width &&
        int64_t(w)*h*best.members >= int64_t(best.width)*best.height*members) return;
    if (members == 2) {
      const auto pa = shiftPolygon(am.rotatedPart,
          {a.x*config.bitmapResolutionMm-am.minX,a.y*config.bitmapResolutionMm-am.minY,true});
      const auto pb = shiftPolygon(bm.rotatedPart,
          {b.x*config.bitmapResolutionMm-bm.minX,b.y*config.bitmapResolutionMm-bm.minY,true});
      if (hasMaterialOverlap(pa,pb,config)) return;
      auto occupied = makeBitmapGrid(w,h);
      auto material = makeBitmapGrid(w,h);
      std::fill(material.bits.begin(),material.bits.end(),~uint64_t(0));
      pixels.ensure(am);
      applyMask(occupied,am,a.x,a.y,false);
      pixels.ensure(bm);
      if (!maskFits(occupied,material,bm,b.x,b.y,false)) return;
    }
    best=p;
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
    const size_t vertexStride=std::max<size_t>(1,(nfp->points.size()+127)/128);
    for (size_t i=0; i<nfp->points.size(); i+=vertexStride) {
      const auto& p=nfp->points[i];
      const int x=int(std::floor((p.x-bm.rotatedPart.points.front().x-am.minX+bm.minX)/config.bitmapResolutionMm));
      const int y=int(std::floor((p.y-bm.rotatedPart.points.front().y-am.minY+bm.minY)/config.bitmapResolutionMm));
      for(int dx:{0,1}) for(int dy:{0,1}) consider({0,0,a},{x+dx,y+dy,b},2);
    }
  }
  return best;
}

PlacementResult placePartsBitmapOnSingleSheet(const Polygon& sheet,
                                              const std::vector<Polygon>& parts,
                                              const Config& config,
                                              ParallelLoop& parallel,
                                              const SearchDeadline& deadline,
                                              bool useAvx2,
                                              BitmapNestingStats* stats,
                                              const BitmapPartProgressCallback& onPartProgress) {
  PlacementResult out;
  out.totalarea = polygonMaterialArea(sheet);

  Bounds sheetBounds;
  BitmapNestingStats localStats;
  const auto preparationStart=std::chrono::steady_clock::now();
  BitmapGrid material;
  try { material=rasterizeSheetMaterial(sheet, config.bitmapResolutionMm, sheetBounds,deadline); }
  catch(const SearchTimeExpired&) {
    out.unplaced=parts;
    localStats.timeLimitReached=true;
    if(stats) *stats=localStats;
    return out;
  }
  BitmapGrid occupancy = makeBitmapGrid(material.widthPx, material.heightPx);

  // Heap-own masks so pointers in rotationMasks/rasterPlacements stay valid
  // even when the lookup table grows to thousands of requested angles.
  std::unordered_map<std::string, std::unique_ptr<RasterMask>> maskCache;
  std::unordered_map<std::string, std::vector<const RasterMask*>> rotationCache;
  RasterPixels pixels(config.bitmapResolutionMm,deadline);
  NfpCache contactNfpCache;
  std::vector<std::unordered_map<NfpKey, std::vector<Point>, NfpKeyHash>> workerContacts(parallel.size());
  std::vector<std::vector<Candidate>> workerCandidates(parallel.size());
  std::vector<size_t> workerSkipped(parallel.size());
  std::vector<Polygon> placedAbsolute;
  std::vector<Bounds> placedBounds;
  std::vector<Placement> placements;
  SpatialIndex neighbours(std::max(config.bitmapResolutionMm,
      std::max(sheetBounds.width, sheetBounds.height) / 32.0));
  std::vector<RasterPlacement> rasterPlacements;
  std::unordered_map<std::string, std::vector<RasterPlacement>> repeated;
  std::unordered_set<std::string> exhausted;
  std::unordered_map<std::string, uint64_t> fineSearchCursor;
  std::unordered_map<std::string, RejectedOrigins> rejectedOrigins;
  size_t rejectedCacheBytes = 0;
  std::unordered_map<std::string, size_t> repetitions;
  std::unordered_map<std::string, PairPattern> patterns;
  if (config.bitmapPatternTrial)
    for (const auto& p : parts) ++repetitions[searchPolicyKey(p)];
  int usedWidth = 0, usedHeight = 0;
  size_t placedCount = 0;
  size_t unplacedCount = 0;
  const size_t totalParts = parts.size();
  const int searchStep = std::max(1, config.bitmapSearchStepPx);
  std::unique_ptr<GpuBitmap> gpu;
  auto gpuFailed=[&](const std::exception& e) {
    if(!config.gpuFallbackToCpu) throw std::runtime_error(e.what());
    localStats.gpuFallbackReason=e.what();
    gpu.reset();
  };
  if(config.gpuEnabled && !deadline.expired()) {
    try {
      gpu=std::make_unique<GpuBitmap>(config.gpuDevice);
      localStats.gpuDevice=gpu->device().name;
      gpu->setSheet(material.widthPx,material.heightPx,material.bits);
    } catch(const std::exception& e) { gpuFailed(e); }
  }
  std::string gpuMaskIdentity;
  size_t gpuOccupancyVersion=std::numeric_limits<size_t>::max();
  const auto bitmapStart = std::chrono::steady_clock::now();
  localStats.phases.preparationMs=std::chrono::duration<double,std::milli>(bitmapStart-preparationStart).count();

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
    try {

    const int rotationCount = part.allowedAngles.empty() ? config.rotations : int(part.allowedAngles.size());
    const double rotationStep = 360.0 / static_cast<double>(rotationCount);
    const std::string searchIdentity = searchPolicyKey(part);
    auto [rotationIt,newRotations]=rotationCache.try_emplace(searchIdentity);
    auto& rotationMasks=rotationIt->second;
    if(newRotations) {
      PhaseTimer timer{localStats.phases.preparationMs};
      rotationMasks.reserve(static_cast<size_t>(rotationCount));
      for (int r = 0; r < rotationCount; ++r) {
        deadline.check();
        const auto angleIndex=(size_t(r)+size_t(config.searchIteration%uint64_t(rotationCount)))%size_t(rotationCount);
        const double rotation = part.allowedAngles.empty() ? part.rotation + rotationStep * static_cast<double>(angleIndex) : part.allowedAngles[angleIndex];
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

    auto prepareGpu=[&] {
      deadline.check();
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
      if(!gpu) return std::nullopt;
      PhaseTimer timer{localStats.phases.gpuMs};
      try {
        prepareGpu();
        auto flags=gpu->filterGrid(first,count,rows,step,w,h);
        ++localStats.gpuBatches; localStats.gpuCandidates+=count;
        deadline.check();
        return flags;
      } catch(const std::exception& e) { gpuFailed(e); return std::nullopt; }
    };
    auto& rejected = rejectedOrigins[searchIdentity];
    if (rejected.rows == 0 && globalMaxOriginX >= 0 && globalMaxOriginY >= 0) {
      rejected.rows = uint64_t(globalMaxOriginY) + 1;
      rejected.rotations = rotationMasks.size();
      rejected.size = (uint64_t(globalMaxOriginX) + 1) * rejected.rows * rejected.rotations;
      const uint64_t bytes = ((rejected.size + 63) / 64) * sizeof(uint64_t);
      constexpr size_t budget = 32ULL * 1024 * 1024;
      if (config.bitmapCacheRejects && bytes <= budget - rejectedCacheBytes) {
        rejected.bits.assign(size_t((rejected.size + 63) / 64), 0);
        rejectedCacheBytes += size_t(bytes);
      }
    }
    using Score = std::tuple<int64_t, int, int, int, size_t>;
    auto score = [&](const Candidate& c) -> Score {
      const auto* m = rotationMasks[c.rotation];
      const int w = std::max(usedWidth, c.x + m->widthPx);
      const int h = std::max(usedHeight, c.y + m->heightPx);
      return {int64_t(w) * h, std::max(w, h), c.x, c.y, c.rotation};
    };
    std::optional<Score> bestScore;
    auto evaluate = [&](const Candidate& c) {
      if((partCandidatesExamined & 255)==0) deadline.check();
      const int x = c.x, y = c.y;
      const RasterMask* mask = rotationMasks[c.rotation];
      ++partCandidatesExamined;
      if (x < 0 || y < 0 || x > material.widthPx - mask->widthPx ||
          y > material.heightPx - mask->heightPx) {
        ++partBoundaryRejects;
        return false;
      }
      const Score candidateScore = score(c);
      if (bestScore && candidateScore >= *bestScore) return false;
      const uint64_t origin = rejected.index(x, y, c.rotation);
      if (rejected.contains(origin)) {
        ++localStats.rejectedPositionSkips;
        return false;
      }
      pixels.ensure(*mask);
      if (!maskFits(occupancy, material, *mask, x, y, useAvx2)) {
        rejected.insert(origin);
        ++partBitmapCollisions;
        return false;
      }
      const double shiftX = sheetBounds.x + x * config.bitmapResolutionMm - mask->minX;
      const double shiftY = sheetBounds.y + y * config.bitmapResolutionMm - mask->minY;
      const Bounds absoluteBounds{sheetBounds.x + x * config.bitmapResolutionMm,
                                  sheetBounds.y + y * config.bitmapResolutionMm,
                                  mask->rotatedBounds.width, mask->rotatedBounds.height};
      if (config.bitmapValidateGeometry) {
        Polygon absolute = shiftPolygon(mask->rotatedPart, {shiftX, shiftY, true});
        if (hasMaterialOutsideSheet(absolute, sheet, config)) {
          rejected.insert(origin);
          ++partVectorValidationRejects;
          return false;
        }
        for (size_t id : neighbours.query(absoluteBounds)) {
          if (!boundsIntersect(absoluteBounds, placedBounds[id])) continue;
          ++localStats.neighbourGeometryChecks;
          if (hasMaterialOverlap(absolute, placedAbsolute[id], config)) {
            rejected.insert(origin);
            ++partVectorValidationRejects;
            return false;
          }
        }
        acceptedAbsolute = std::move(absolute);
        acceptedBounds = absoluteBounds;
      }
      bestScore = candidateScore;
      acceptedPlacement = Placement{shiftX, shiftY, part.id, mask->rotationDeg,
                                    part.source, part.filename, 0.0, {}};
      acceptedMask = mask;
      acceptedX = x;
      acceptedY = y;
      return true;
    };

    int maxWidth = 1, maxHeight = 1;
    for (const auto* m : rotationMasks) {
      maxWidth = std::max(maxWidth, m->widthPx);
      maxHeight = std::max(maxHeight, m->heightPx);
    }
    bool fromPattern=false;
    if (config.bitmapPatternTrial && repetitions[searchIdentity] >= 6) {
      auto [patternIt,inserted] = patterns.try_emplace(searchIdentity);
      if (inserted) patternIt->second=makePairPattern(rotationMasks,material.widthPx,material.heightPx,config,contactNfpCache,pixels,deadline);
      auto& pattern=patternIt->second;
      while (pattern.cursor < pattern.capacity()) {
        if (evaluate(pattern.next())) { fromPattern=true; ++localStats.patternPlacements; break; }
      }
    }
    std::vector<Candidate> candidates;
    if (!fromPattern && !exhausted.contains(searchIdentity)) {
      PhaseTimer proposalTimer{localStats.phases.proposalsMs};
      parallel.run(rotationMasks.size(),[&](size_t begin,size_t end,size_t slot) {
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
      for (size_t r = begin; r < end; ++r) {
        deadline.check();
        const size_t first=localCandidates.size();
        const auto* m = rotationMasks[r];
        propose(0, 0, r);
        for (size_t i = frontierStart; i < rasterPlacements.size(); ++i) {
          const auto& q = rasterPlacements[i];
          const int right = q.x + q.mask->widthPx, top = q.y + q.mask->heightPx;
          // Bounding-box contacts cheaply grow rows and columns around the occupied region.
          propose(right, q.y, r); propose(q.x, top, r);
          propose(right - m->widthPx, top, r); propose(right, top - m->heightPx, r);
          propose(q.x - m->widthPx, q.y, r); propose(q.x, q.y - m->heightPx, r);
          // A bounded recent frontier adds interlocking proposals for concave parts.
          if (i + 24 >= rasterPlacements.size() && r % contactStride == 0) {
            if (q.identity == identity) {
              // Compute tight pair contacts once per repeated shape/rotation pair.
              // Unlike full NFP placement, no union of all occupied NFPs is built.
              NfpKey key{identity, identity, q.mask->rotationDeg, m->rotationDeg, false};
              auto [it, inserted] = pairContacts.try_emplace(key);
              if (inserted) {
                auto nfp = getOuterNfp(q.mask->rotatedPart, m->rotatedPart, false, config, contactNfpCache);
                if (nfp) {
                  for (const auto& p : nfp->points)
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
        if (it != repeated.end() && it->second.size() >= 2) {
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
    }
    if (!fromPattern && !exhausted.contains(searchIdentity)) {
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
        for (size_t i=0; i<limit; ++i) {
          evaluate(inWindow[i]);
          if (bestScore) break;
        }
        if (!bestScore) {
          // Coarse search is bounded to roughly 1024 grid points per window/rotation.
          const int coarse = std::max(searchStep, static_cast<int>(std::ceil(
              std::sqrt(static_cast<double>(windowW) * windowH / 1024.0))));
          if(!gpu) {
            for(int x=0;x<windowW;x+=coarse) for(int y=0;y<windowH;y+=coarse)
              for(size_t r=0;r<rotationMasks.size();++r)
                if(x+rotationMasks[r]->widthPx<=windowW && y+rotationMasks[r]->heightPx<=windowH) evaluate({x,y,r});
          } else {
          const uint64_t rows=(uint64_t(windowH)+coarse-1)/coarse;
          const uint64_t end=((uint64_t(windowW)+coarse-1)/coarse)*rows*rotationMasks.size();
          for(uint64_t first=0;first<end;) {
            deadline.check();
            const uint32_t count=uint32_t(std::min<uint64_t>(config.gpuBatchSize,end-first));
            const auto flags=gpuGrid(first,count,uint32_t(rows),coarse,windowW,windowH);
            for(uint32_t i=0;i<count;++i) {
              if(flags && !(*flags)[i]) continue;
              const uint64_t p=first+i;
              const size_t r=size_t(p%rotationMasks.size());
              const int x=int(p/rotationMasks.size()/rows)*coarse;
              const int y=int((p/rotationMasks.size())%rows)*coarse;
              if(x+rotationMasks[r]->widthPx<=windowW && y+rotationMasks[r]->heightPx<=windowH) evaluate({x,y,r});
            }
            first+=count;
          }
          }
        }
        if (bestScore || (windowW == material.widthPx && windowH == material.heightPx)) break;
        ++localStats.searchWindowExpansions;
        windowW = std::min(material.widthPx, std::max(windowW + maxWidth, windowW * 2));
        windowH = std::min(material.heightPx, std::max(windowH + maxHeight, windowH * 2));
      }
      // Fine fallback preserves small feasible pockets missed by contact/coarse proposals.
      // It runs only after the fast search fails; identical exhausted shapes skip it later.
      if (!bestScore) {
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
            if(gpu) {
              const uint32_t count=uint32_t(std::min<uint64_t>(config.gpuBatchSize,end-cursor));
              const auto flags=gpuGrid(cursor,count,uint32_t(rows),searchStep,material.widthPx,material.heightPx);
              if(flags) {
                for(uint32_t i=0;i<count && !bestScore;++i) {
                  const bool found=(*flags)[i] && evaluate({int(cursor/rotations/rows)*searchStep,
                      int((cursor/rotations)%rows)*searchStep,size_t(cursor%rotations)});
                  if(!found) ++cursor;
                }
                continue;
              }
            }
            if (searchStep == 1) {
              const auto next = rejected.next(cursor);
              localStats.rejectedPositionSkips += size_t(next - cursor);
              cursor = next;
              if (cursor >= end) break;
            }
            const uint64_t position = cursor;
            const bool found = evaluate({static_cast<int>(position / rotations / rows) * searchStep,
                      static_cast<int>((position / rotations) % rows) * searchStep,
                      static_cast<size_t>(position % rotations)});
            // Compaction may move a successful placement away from its first
            // origin. Only a failed origin is a proven invalid prefix.
            if (!found) ++cursor;
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
            for (int dx : {-step, 0, step})
              for (int dy : {-step, 0, step})
                for (size_t r = 0; r < rotationMasks.size(); ++r)
                  evaluate({startX + dx, startY + dy, r});
            if (*bestScore == previousScore) break;
          }
          if (step == searchStep) break;
        }
      } else {
        if (searchStep == 1) exhausted.insert(searchIdentity);
      }
    } else if (!fromPattern) {
      ++localStats.exhaustedShapeSkips;
    }
    } catch(const SearchTimeExpired&) {
      // Keep the best fully validated candidate even if refinement was stopped.
      localStats.timeLimitReached=true;
    }

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - partStart).count();

    if (!acceptedPlacement.has_value() || acceptedMask == nullptr) {
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
    RasterPlacement raster{acceptedX, acceptedY, acceptedMask, identity};
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

}  // namespace

bool bitmapAvx2Supported() {
#if defined(__AVX2__)
  return avx2RuntimeAvailable();
#else
  return false;
#endif
}

PlacementResult placePartsBitmap(const std::vector<Polygon>& sheets,
                                 const std::vector<Polygon>& parts,
                                 const Config& config,
                                 BitmapNestingStats* stats,
                                 const BitmapPartProgressCallback& onPartProgress,
                                 const BitmapLayoutCallback& onLayout) {
  if (config.rotations < 1 || config.rotations > Config::maxRotations) {
    throw std::invalid_argument("rotations must be an integer from 1 to 3600");
  }
  if(config.gpuBatchSize<256 || config.gpuBatchSize>262144 || config.gpuDevice < -1)
    throw std::invalid_argument("Invalid GPU configuration");
  if (!std::isfinite(config.bitmapResolutionMm) || config.bitmapResolutionMm <= 0.0) {
    throw std::invalid_argument("--bitmap-resolution must be a positive number");
  }
  if (config.bitmapSearchStepPx <= 0) {
    throw std::invalid_argument("--bitmap-step must be a positive integer");
  }
  for(const auto& part:parts) {
    if(part.allowedAngles.size()>Config::maxRotations)
      throw std::invalid_argument("At most 3600 allowedAngles per part are supported");
    for(double angle:part.allowedAngles)
      if(!std::isfinite(angle) || angle<0 || angle>=360)
        throw std::invalid_argument("Library allowedAngles must be normalized to [0,360)");
  }
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
  const int trialCount=parts.size()<6 ? 1 : std::clamp(config.bitmapTrials,1,4);
  const int workerCount=std::min(trialCount,normalizeWorkerCount(config.threads));
  const bool fineAngles=std::any_of(parts.begin(),parts.end(),[&](const Polygon& p) {
    return (p.allowedAngles.empty() ? size_t(config.rotations) : p.allowedAngles.size())>64;
  });
  const int helpersPerTrial=fineAngles ? std::max(1,std::min(defaultWorkerCount(),normalizeWorkerCount(config.threads))/workerCount) : 1;
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
        cfg.bitmapPatternTrial=i==1;
        auto remaining=parts;
        if(config.searchIteration>0) {
          std::mt19937_64 random(config.searchIteration*0x9e3779b97f4a7c15ULL+uint64_t(i));
          std::shuffle(remaining.begin(),remaining.end(),random);
          cfg.searchIteration=random();
        } else if (i>=2) std::stable_sort(remaining.begin(),remaining.end(),[&](const Polygon& a,const Polygon& b) {
          return i==2 ? polygonMaterialArea(a)>polygonMaterialArea(b) : polygonMaterialArea(a)<polygonMaterialArea(b);
        });
        auto& trial=trials[size_t(i)];
        trial.started=true;
        for (const auto& sheet:sheets) {
          if (remaining.empty()) break;
          if(deadline.expired()) { trial.stats.timeLimitReached=true; break; }
          BitmapNestingStats s;
          auto r=placePartsBitmapOnSingleSheet(sheet,remaining,cfg,parallel,deadline,useAvx2,&s,{});
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
          t.rejectedPositionSkips+=s.rejectedPositionSkips;
          t.patternPlacements+=s.patternPlacements;
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
    return std::tuple{t.result.unplaced.size(),t.result.placements.size(),t.stats.occupiedBoundsArea};
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
  const char* strategyNames[]={"compact","pair_rows","large_first","small_first"};
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
  winner.stats.completedTrials=completedTrials;
  winner.stats.cancelled=deadline.cancelled();
  winner.stats.timeLimitReached=completedTrials<size_t(trialCount) && deadline.timedOut();
  winner.stats.workersUsed=workerCount;
  winner.stats.proposalWorkersPerTrial=helpersPerTrial;
  winner.stats.simdBackend=useAvx2 ? "avx2" : "scalar";
  winner.stats.totalBitmapMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  // Only publish the selected result, on the caller thread. Worker callbacks
  // never race each other or expose placements from a discarded strategy.
  if(onPartProgress) for(const auto& p:winner.stats.perPart) onPartProgress(p,winner.stats.perPart.size());
  if(stats) *stats=winner.stats;
  return std::move(winner.result);
}

}  // namespace deepnest
