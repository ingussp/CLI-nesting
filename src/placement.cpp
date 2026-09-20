#include "clinesting/placement.hpp"

#include "clinesting/geometry.hpp"
#include "clinesting/nfp.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace clinesting {

using namespace Clipper2Lib;

namespace {

// Apply each part's initial orientation before reference NFP placement.
std::vector<Polygon> rotateParts(const std::vector<Polygon>& parts) {
  std::vector<Polygon> out;
  out.reserve(parts.size());
  for (const auto& p : parts) {
    Polygon r = rotatePolygon(p, p.rotation);
    r.rotation = p.rotation;
    r.source = p.source;
    r.id = p.id;
    r.filename = p.filename;
    out.push_back(std::move(r));
  }
  return out;
}

// Build a placement record from a part and its translation.
Placement toPlacement(const Point& shift, const Polygon& part) {
  Placement p;
  p.x = shift.x;
  p.y = shift.y;
  p.id = part.id;
  p.rotation = part.rotation;
  p.source = part.source;
  p.filename = part.filename;
  return p;
}

// Compute the smallest rectangle enclosing two bounds.
Bounds mergeBounds(const Bounds& a, const Bounds& b) {
  const double minx = std::min(a.x, b.x);
  const double miny = std::min(a.y, b.y);
  const double maxx = std::max(a.x + a.width, b.x + b.width);
  const double maxy = std::max(a.y + a.height, b.y + b.height);
  return Bounds{minx, miny, maxx - minx, maxy - miny};
}

// Convert a forbidden region while removing its feasible holes.
Paths64 forbiddenNfpToClipperCoordinates(const Polygon& nfp, const Config& config) {
  Paths64 forbidden{outerPathToClipperCoordinates(nfp, config)};
  auto holes = childPathsToClipperCoordinates(nfp, config);
  if (!holes.empty()) {
    forbidden = Difference(forbidden, holes, FillRule::NonZero);
  }
  return forbidden;
}

}  // namespace

// Place parts with the reference no-fit-polygon algorithm.
PlacementResult placeParts(std::vector<Polygon> sheets,
                           std::vector<Polygon> parts,
                           const Config& config,
                           NfpCache& cache,
                           ProgressCallback progress) {
  if(config.timeLimitSeconds!=0 || std::any_of(parts.begin(),parts.end(),
      [](const Polygon& p) { return !p.allowedAngles.empty(); }))
    throw std::invalid_argument("Per-part angles and time limits require algorithm: bitmap");
  if(config.spacing>0 || config.sheetSpacing>0 || config.holeSpacing>0)
    throw std::invalid_argument("Clearance settings require algorithm: bitmap");
  PlacementResult out;
  if (sheets.empty()) {
    return out;
  }

  parts = rotateParts(parts);

  const int totalnum = static_cast<int>(parts.size());
  double totalsheetarea = 0.0;
  double totalusablesheetarea = 0.0;
  double totalplacedarea = 0.0;
  double totalMerged = 0.0;
  double fitness = 0.0;

  while (!parts.empty() && !sheets.empty()) {
    std::vector<Polygon> placed;
    std::vector<Polygon> placedAbsolute;
    std::vector<Placement> placements;
    std::vector<size_t> placedIndices;
    std::vector<Point> placedPoints;
    Bounds placedBounds;
    bool hasPlacedBounds = false;
    // Cache per unique placed-shape/candidate-shape rotation pair on the current sheet.
    // Every placed instance still contributes its own translated exclusion region, but we
    // avoid rebuilding the same outer-NFP clipper geometry for identical copies.
    std::unordered_map<NfpKey, Paths64, NfpKeyHash> forbiddenPairCache;

    Polygon sheet = sheets.front();
    sheets.erase(sheets.begin());

    const double sheetarea = std::abs(polygonArea(sheet));
    totalsheetarea += sheetarea;
    totalusablesheetarea += polygonMaterialArea(sheet);
    fitness += sheetarea;

    double minwidth = 0.0;
    double minarea = 0.0;
    bool hasSheetPlacementScore = false;

    for (size_t i = 0; i < parts.size(); ++i) {
      Polygon part = parts[i];

      std::optional<std::vector<Polygon>> sheetNfp;
      for (int rAttempt = 0; rAttempt < std::max(1, config.rotations); ++rAttempt) {
        sheetNfp = getInnerNfp(sheet, part, config, cache);
        if (sheetNfp.has_value()) {
          break;
        }

        Polygon rotated = rotatePolygon(part, 360.0 / std::max(1, config.rotations));
        rotated.rotation = part.rotation + (360.0 / std::max(1, config.rotations));
        if (rotated.rotation > 360.0) {
          rotated.rotation = std::fmod(rotated.rotation, 360.0);
        }
        rotated.source = part.source;
        rotated.id = part.id;
        rotated.filename = part.filename;

        part = rotated;
        parts[i] = rotated;
      }

      if (!sheetNfp.has_value() || sheetNfp->empty()) {
        continue;
      }

      std::optional<Placement> bestPosition;

      if (placed.empty()) {
        for (const auto& nfpPoly : *sheetNfp) {
          for (const auto& nfpPt : nfpPoly.points) {
            Point shift{nfpPt.x - part.points.front().x, nfpPt.y - part.points.front().y, true};
            Polygon shifted = shiftPolygon(part, shift);
            if (hasMaterialOutsideSheet(shifted, sheet, config)) {
              continue;
            }

            if (!bestPosition.has_value() || shift.x < bestPosition->x ||
                (almostEqual(shift.x, bestPosition->x) && shift.y < bestPosition->y)) {
              bestPosition = toPlacement(shift, part);
            }
          }
        }

        if (!bestPosition.has_value()) {
          continue;
        }
        placements.push_back(*bestPosition);
        placed.push_back(part);
        placedIndices.push_back(i);
        Polygon absolute = shiftPolygon(part, {bestPosition->x, bestPosition->y, true});
        placedAbsolute.push_back(absolute);
        if (config.placementType == "convexhull") {
          placedPoints.insert(placedPoints.end(), absolute.points.begin(), absolute.points.end());
        }
        placedBounds = getPolygonBounds(absolute.points);
        hasPlacedBounds = true;
        continue;
      }

      Paths64 finalNfp = innerNfpToClipperCoordinates(*sheetNfp, config);
      bool error = false;
      Paths64 occupiedForbidden;

      for (size_t j = 0; j < placed.size(); ++j) {
        NfpKey pairKey{polygonGeometryIdentity(placed[j]), polygonGeometryIdentity(part), placed[j].rotation, part.rotation,
                       false};
        auto cacheIt = forbiddenPairCache.find(pairKey);
        if (cacheIt == forbiddenPairCache.end()) {
          auto nfpOpt = getOuterNfp(placed[j], part, false, config, cache);
          if (!nfpOpt.has_value()) {
            error = true;
            break;
          }
          cacheIt = forbiddenPairCache.emplace(pairKey, forbiddenNfpToClipperCoordinates(*nfpOpt, config)).first;
        }

        const int64_t dx = static_cast<int64_t>(std::llround(placements[j].x * config.clipperScale));
        const int64_t dy = static_cast<int64_t>(std::llround(placements[j].y * config.clipperScale));
        auto translated = translatePaths(cacheIt->second, dx, dy);
        occupiedForbidden.insert(occupiedForbidden.end(), translated.begin(), translated.end());
      }

      // Subtract all translated forbidden regions in one clipper call. This preserves
      // correctness while avoiding repeated Difference/Union passes for identical copies.
      if (!error && !occupiedForbidden.empty()) {
        finalNfp = Difference(finalNfp, occupiedForbidden, FillRule::NonZero);
      }
      if (error || finalNfp.empty()) {
        continue;
      }

      std::vector<Polygon> validRegions;
      for (const auto& path : finalNfp) {
        if (std::abs(Area(path)) <= 0.0) {
          continue;
        }
        Polygon poly;
        poly.points = toNestCoordinates(path, config.clipperScale);
        validRegions.push_back(std::move(poly));
      }
      if (validRegions.empty()) {
        continue;
      }

      std::optional<double> localMinWidth;
      std::optional<double> localMinArea;
      std::optional<double> localMinX;

      Bounds allbounds;
      Bounds partbounds;
      std::vector<Point> hull;
      if (config.placementType == "gravity" || config.placementType == "box") {
        allbounds = hasPlacedBounds ? placedBounds : Bounds{};
        partbounds = getPolygonBounds(part.points);
      } else if (config.placementType == "convexhull" && !placedPoints.empty()) {
        hull = getHull(placedPoints);
      }

      for (const auto& region : validRegions) {
        for (const auto& v : region.points) {
          Point shift{v.x - part.points.front().x, v.y - part.points.front().y, true};
          Placement candidate = toPlacement(shift, part);
          double areaScore = 0.0;
          double widthScore = 0.0;

          if (config.placementType == "gravity" || config.placementType == "box") {
            Bounds rect = getPolygonBounds({
                {allbounds.x, allbounds.y, true},
                {allbounds.x + allbounds.width, allbounds.y, true},
                {allbounds.x + allbounds.width, allbounds.y + allbounds.height, true},
                {allbounds.x, allbounds.y + allbounds.height, true},
                {partbounds.x + shift.x, partbounds.y + shift.y, true},
                {partbounds.x + partbounds.width + shift.x, partbounds.y + shift.y, true},
                {partbounds.x + partbounds.width + shift.x, partbounds.y + partbounds.height + shift.y, true},
                {partbounds.x + shift.x, partbounds.y + partbounds.height + shift.y, true},
            });

            widthScore = rect.width;
            if (config.placementType == "gravity") {
              areaScore = rect.width * 5.0 + rect.height;
            } else {
              areaScore = rect.width * rect.height;
            }
          } else {
            std::vector<Point> partPoints;
            partPoints.reserve(part.points.size());
            for (const auto& pt : part.points) {
              partPoints.push_back({pt.x + shift.x, pt.y + shift.y, true});
            }
            std::vector<Point> combined = placedPoints.empty() ? partPoints : hull;
            combined.insert(combined.end(), partPoints.begin(), partPoints.end());
            auto combinedHull = getHull(combined);
            if (combinedHull.empty()) {
              continue;
            }
            areaScore = std::abs(polygonArea(combinedHull));
          }

          if (config.mergeLines) {
            Polygon shiftedPart = shiftPolygon(part, shift);
            const double minlength = 0.5 * config.scale;
            auto merged = mergedLength(placedAbsolute, shiftedPart, minlength, 0.1 * config.curveTolerance);
            areaScore -= merged.totalLength * config.timeRatio;
            candidate.mergedLength = merged.totalLength;
            candidate.mergedSegments = merged.segments;
          }

          bool prefer = !localMinArea.has_value();
          if (!prefer && config.placementType == "gravity") {
            prefer = widthScore < localMinWidth.value() ||
                     (almostEqual(widthScore, localMinWidth.value()) && areaScore < localMinArea.value()) ||
                     (almostEqual(areaScore, localMinArea.value()) && shift.x < localMinX.value());
          } else if (!prefer) {
            prefer = areaScore < localMinArea.value() ||
                     (almostEqual(areaScore, localMinArea.value()) && shift.x < localMinX.value());
          }

          if (prefer) {
            Polygon testShifted = shiftPolygon(part, shift);
            bool overlapping = hasMaterialOutsideSheet(testShifted, sheet, config);
            for (size_t m = 0; !overlapping && m < placed.size(); ++m) {
              if (hasMaterialOverlap(testShifted, placedAbsolute[m], config)) {
                overlapping = true;
              }
            }

            if (!overlapping) {
              localMinArea = areaScore;
              localMinX = shift.x;
              localMinWidth = widthScore;
              bestPosition = candidate;
            }
          }
        }
      }

      if (bestPosition.has_value()) {
        placed.push_back(part);
        placements.push_back(*bestPosition);
        placedIndices.push_back(i);
        Polygon absolute = shiftPolygon(part, {bestPosition->x, bestPosition->y, true});
        placedAbsolute.push_back(absolute);
        if (config.placementType == "convexhull") {
          placedPoints.insert(placedPoints.end(), absolute.points.begin(), absolute.points.end());
        }
        const Bounds absoluteBounds = getPolygonBounds(absolute.points);
        placedBounds = hasPlacedBounds ? mergeBounds(placedBounds, absoluteBounds) : absoluteBounds;
        hasPlacedBounds = true;
        totalMerged += bestPosition->mergedLength;
        minwidth = localMinWidth.value_or(0.0);
        minarea = localMinArea.value_or(0.0);
        hasSheetPlacementScore = true;
      }

      int placednum = static_cast<int>(placed.size());
      for (const auto& s : out.placements) {
        placednum += static_cast<int>(s.sheetplacements.size());
      }
      if (progress) {
        progress(0.5 + 0.5 * (static_cast<double>(placednum) / std::max(1, totalnum)));
      }
    }

    if (hasSheetPlacementScore) {
      fitness += (sheetarea > 0.0 ? (minwidth / sheetarea) : 0.0) + minarea;
    }

    for (const auto& placedPart : placed) {
      totalplacedarea += polygonMaterialArea(placedPart);
    }

    std::sort(placedIndices.begin(), placedIndices.end());
    placedIndices.erase(std::unique(placedIndices.begin(), placedIndices.end()), placedIndices.end());
    for (auto it = placedIndices.rbegin(); it != placedIndices.rend(); ++it) {
      if (*it < parts.size()) {
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(*it));
      }
    }

    if (!placements.empty()) {
      out.placements.push_back(SheetPlacement{sheet.source, sheet.id, placements});
    } else {
      break;
    }
  }

  for (const auto& part : parts) {
    double penalty = 100000000.0 * ((std::abs(polygonArea(part)) * 100.0) / std::max(1.0, totalsheetarea));
    fitness += penalty;
    out.unplaced.push_back(part);
  }

  if (progress) {
    progress(-1.0);
  }

  out.fitness = fitness;
  out.area = totalplacedarea;
  out.totalarea = totalusablesheetarea;
  out.mergedLength = totalMerged;
  out.utilisation = totalusablesheetarea > 0.0 ? (totalplacedarea / totalusablesheetarea) * 100.0 : 0.0;
  return out;
}

}  // namespace clinesting
