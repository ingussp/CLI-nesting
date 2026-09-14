#include "deepnestcpp/nfp.hpp"

#include "deepnestcpp/geometry.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace deepnest {

using namespace Clipper2Lib;

namespace {

Polygon pathToPolygon(const Path64& p, double scale) {
  Polygon poly;
  poly.points = toNestCoordinates(p, scale);
  return poly;
}

Bounds getBounds(const Polygon& p) {
  return getPolygonBounds(p.points);
}

std::optional<Polygon> minkowskiOuter(const Polygon& A, const Polygon& B, const Config& config) {
  if (A.points.empty() || B.points.empty()) {
    return std::nullopt;
  }

  auto Ac = toClipperCoordinates(A.points, config.clipperScale);
  auto Bc = toClipperCoordinates(B.points, config.clipperScale);
  for (auto& p : Bc) {
    p.x *= -1;
    p.y *= -1;
  }

  Paths64 solution = MinkowskiSum(Ac, Bc, true);
  if (solution.empty()) {
    return std::nullopt;
  }

  Path64 best;
  double bestArea = -1.0;
  for (const auto& s : solution) {
    double area = std::abs(Area(s));
    if (area > bestArea) {
      bestArea = area;
      best = s;
    }
  }

  if (best.empty()) {
    return std::nullopt;
  }

  Polygon nfp = pathToPolygon(best, config.clipperScale);
  const auto& b0 = B.points.front();
  for (auto& pt : nfp.points) {
    pt.x += b0.x;
    pt.y += b0.y;
  }
  return nfp;
}

Paths64 materialContainerPaths(const Polygon& A, const Config& config) {
  Paths64 outer{outerPathToClipperCoordinates(A, config)};
  auto holes = childPathsToClipperCoordinates(A, config);
  if (holes.empty()) {
    return outer;
  }
  return Difference(outer, holes, FillRule::NonZero);
}

}  // namespace

Polygon getFrame(const Polygon& A) {
  Bounds bounds = getBounds(A);
  const double originalW = bounds.width;
  const double originalH = bounds.height;
  bounds.width *= 1.1;
  bounds.height *= 1.1;
  bounds.x -= 0.5 * (bounds.width - originalW);
  bounds.y -= 0.5 * (bounds.height - originalH);

  Polygon frame;
  frame.points = {
      {bounds.x, bounds.y, true},
      {bounds.x + bounds.width, bounds.y, true},
      {bounds.x + bounds.width, bounds.y + bounds.height, true},
      {bounds.x, bounds.y + bounds.height, true},
  };
  frame.children.push_back(A);
  frame.source = A.source;
  frame.rotation = 0.0;
  return frame;
}

std::optional<std::vector<Polygon>> getInnerNfp(const Polygon& A,
                                                const Polygon& B,
                                                const Config& config,
                                                NfpCache& cache) {
  const std::string aIdentity = polygonGeometryIdentity(A);
  const std::string bIdentity = polygonGeometryIdentity(B);
  NfpKey key{aIdentity, bIdentity, 0.0, B.rotation, true};
  if (auto cached = cache.findInner(key); cached.has_value()) {
    return cached;
  }

  if (A.points.empty() || B.points.empty()) {
    return std::nullopt;
  }

  const Paths64 container = materialContainerPaths(A, config);
  Paths64 current = container;
  if (current.empty()) {
    return std::nullopt;
  }

  const auto& origin = B.points.front();
  for (const auto& v : B.points) {
    const int64_t dx = static_cast<int64_t>(std::llround((v.x - origin.x) * config.clipperScale));
    const int64_t dy = static_cast<int64_t>(std::llround((v.y - origin.y) * config.clipperScale));
    Paths64 shifted;
    shifted.reserve(current.size());
    shifted.reserve(current.size());
    for (const auto& path : container) {
      Path64 translated;
      translated.reserve(path.size());
      for (const auto& p : path) {
        translated.push_back(Point64{p.x - dx, p.y - dy});
      }
      shifted.push_back(std::move(translated));
    }

    current = Intersect(current, shifted, FillRule::NonZero);
    if (current.empty()) {
      return std::nullopt;
    }
  }

  std::vector<Polygon> result;
  result.reserve(current.size());
  for (const auto& p : current) {
    if (std::abs(Area(p)) <= 0.0) {
      continue;
    }
    result.push_back(pathToPolygon(p, config.clipperScale));
  }

  if (result.empty()) {
    return std::nullopt;
  }

  cache.insertInner(key, result);
  return result;
}

std::optional<Polygon> getOuterNfp(const Polygon& A,
                                   const Polygon& B,
                                   bool inside,
                                   const Config& config,
                                   NfpCache& cache) {
  const std::string aIdentity = polygonGeometryIdentity(A);
  const std::string bIdentity = polygonGeometryIdentity(B);
  NfpKey key{aIdentity, bIdentity, A.rotation, B.rotation, false};
  if (!inside) {
    if (auto cached = cache.findOuter(key); cached.has_value()) {
      return cached;
    }
  }

  auto nfpOpt = minkowskiOuter(A, B, config);
  if (!nfpOpt.has_value()) {
    return std::nullopt;
  }

  Polygon nfp = *nfpOpt;

  if (!inside && !A.children.empty()) {
    Bounds bb = getBounds(B);
    for (const auto& hole : A.children) {
      Bounds hb = getBounds(hole);
      if (hb.width > bb.width && hb.height > bb.height) {
        if (auto inner = getInnerNfp(hole, B, config, cache); inner.has_value()) {
          for (const auto& c : *inner) {
            nfp.children.push_back(c);
          }
        }
      }
    }
  }

  if (!inside) {
    cache.insertOuter(key, nfp);
  }

  return nfp;
}

std::vector<NfpPair> preprocessMissingPairs(const std::vector<Polygon>& parts, NfpCache& cache) {
  std::vector<NfpPair> pairs;
  struct RepresentativePart {
    Polygon polygon;
    std::string identity;
    size_t count{0};
  };

  std::vector<RepresentativePart> representatives;
  representatives.reserve(parts.size());
  std::unordered_map<std::string, size_t> representativeIndexBySignature;

  // Collapse identical geometry+rotation inputs to representatives so the warm-up step
  // scales with unique shapes instead of raw instance count. Each physical instance is
  // still placed later; this only avoids redundant pair discovery and NFP generation.
  for (const auto& part : parts) {
    const std::string identity = polygonGeometryIdentity(part);
    const std::string signature = identity + "|" + std::to_string(part.rotation);
    const auto [it, inserted] = representativeIndexBySignature.emplace(signature, representatives.size());
    if (inserted) {
      representatives.push_back(RepresentativePart{part, identity, 1});
    } else {
      ++representatives[it->second].count;
    }
  }

  for (size_t i = 0; i < representatives.size(); ++i) {
    const auto& B = representatives[i];
    if (B.count > 1) {
      NfpKey sameKey{B.identity, B.identity, B.polygon.rotation, B.polygon.rotation, false};
      if (!cache.has(sameKey)) {
        pairs.push_back(NfpPair{B.polygon, B.polygon, B.polygon.rotation, B.polygon.rotation, B.polygon.source,
                                B.polygon.source});
      }
    }

    for (size_t j = 0; j < i; ++j) {
      const auto& A = representatives[j];
      NfpKey key{A.identity, B.identity, A.polygon.rotation, B.polygon.rotation, false};
      if (cache.has(key)) {
        continue;
      }
      pairs.push_back(
          NfpPair{A.polygon, B.polygon, A.polygon.rotation, B.polygon.rotation, A.polygon.source, B.polygon.source});
    }
  }

  return pairs;
}

}  // namespace deepnest
