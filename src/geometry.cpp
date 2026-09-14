#include "deepnestcpp/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <sstream>

namespace deepnest {

using namespace Clipper2Lib;

namespace {

static Path64 translatePath(const Path64& path, int64_t dx, int64_t dy) {
  Path64 out;
  out.reserve(path.size());
  for (const auto& p : path) {
    out.push_back(Point64{p.x + dx, p.y + dy});
  }
  return out;
}

long long geometryCoordKey(double v) {
  return static_cast<long long>(std::llround(v * 1000000.0));
}

void appendPolygonGeometrySignature(const Polygon& polygon,
                                    const Point& origin,
                                    std::ostringstream& out) {
  out << "p(";
  for (const auto& point : polygon.points) {
    out << geometryCoordKey(point.x - origin.x) << ',' << geometryCoordKey(point.y - origin.y) << ';';
  }
  out << ")c[" << polygon.children.size() << ':';
  for (const auto& child : polygon.children) {
    appendPolygonGeometrySignature(child, origin, out);
  }
  out << ']';
}

}  // namespace

bool almostEqual(double a, double b, double tolerance) {
  return std::abs(a - b) <= tolerance;
}

double polygonArea(const std::vector<Point>& path) {
  if (path.size() < 3) {
    return 0.0;
  }
  double area = 0.0;
  for (size_t i = 0; i < path.size(); ++i) {
    const auto& p1 = path[i];
    const auto& p2 = path[(i + 1) % path.size()];
    area += p1.x * p2.y - p2.x * p1.y;
  }
  return area / 2.0;
}

double polygonArea(const Polygon& polygon) {
  return polygonArea(polygon.points);
}

Bounds getPolygonBounds(const std::vector<Point>& path) {
  Bounds b;
  if (path.empty()) {
    return b;
  }

  double minx = path.front().x;
  double miny = path.front().y;
  double maxx = minx;
  double maxy = miny;

  for (const auto& p : path) {
    minx = std::min(minx, p.x);
    miny = std::min(miny, p.y);
    maxx = std::max(maxx, p.x);
    maxy = std::max(maxy, p.y);
  }

  b.x = minx;
  b.y = miny;
  b.width = maxx - minx;
  b.height = maxy - miny;
  return b;
}

std::string polygonGeometryIdentity(const Polygon& polygon) {
  if (!polygon.geometryKey.empty()) {
    return polygon.geometryKey;
  }
  if (polygon.points.empty()) {
    return "empty";
  }

  std::ostringstream out;
  appendPolygonGeometrySignature(polygon, polygon.points.front(), out);
  return out.str();
}

Polygon shiftPolygon(const Polygon& p, const Point& shift) {
  Polygon shifted = p;
  shifted.points.clear();
  shifted.points.reserve(p.points.size());
  for (const auto& pt : p.points) {
    shifted.points.push_back(Point{pt.x + shift.x, pt.y + shift.y, pt.exact});
  }
  shifted.children.clear();
  shifted.children.reserve(p.children.size());
  for (const auto& child : p.children) {
    shifted.children.push_back(shiftPolygon(child, shift));
  }
  return shifted;
}

Polygon rotatePolygon(const Polygon& polygon, double degrees) {
  Polygon rotated = polygon;
  rotated.points.clear();
  rotated.points.reserve(polygon.points.size());

  const double angle = degrees * std::numbers::pi_v<double> / 180.0;
  const double c = std::cos(angle);
  const double s = std::sin(angle);

  for (const auto& pt : polygon.points) {
    rotated.points.push_back(Point{pt.x * c - pt.y * s, pt.x * s + pt.y * c, pt.exact});
  }

  rotated.children.clear();
  rotated.children.reserve(polygon.children.size());
  for (const auto& child : polygon.children) {
    rotated.children.push_back(rotatePolygon(child, degrees));
  }

  return rotated;
}

std::vector<Point> clonePolygonPath(const std::vector<Point>& polygon) {
  return polygon;
}

Polygon clonePolygonWithChildren(const Polygon& polygon) {
  return polygon;
}

double polygonMaterialArea(const Polygon& polygon) {
  double material = std::abs(polygonArea(polygon));
  for (const auto& child : polygon.children) {
    material -= std::abs(polygonArea(child));
  }
  return std::max(0.0, material);
}

std::vector<Point> getHull(const std::vector<Point>& polygon) {
  if (polygon.size() <= 2) {
    return polygon;
  }

  std::vector<Point> pts = polygon;
  std::sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) {
    if (a.x == b.x) {
      return a.y < b.y;
    }
    return a.x < b.x;
  });

  auto cross = [](const Point& o, const Point& a, const Point& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };

  std::vector<Point> hull;
  hull.reserve(pts.size() * 2);

  for (const auto& p : pts) {
    while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull[hull.size() - 1], p) <= 0.0) {
      hull.pop_back();
    }
    hull.push_back(p);
  }

  size_t lower = hull.size();
  for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
    while (hull.size() > lower && cross(hull[hull.size() - 2], hull[hull.size() - 1], *it) <= 0.0) {
      hull.pop_back();
    }
    hull.push_back(*it);
  }

  if (!hull.empty()) {
    hull.pop_back();
  }
  return hull.empty() ? polygon : hull;
}

std::vector<Point64> toClipperCoordinates(const std::vector<Point>& polygon, double scale) {
  std::vector<Point64> out;
  out.reserve(polygon.size());
  for (const auto& p : polygon) {
    out.push_back(Point64{static_cast<int64_t>(std::llround(p.x * scale)),
                          static_cast<int64_t>(std::llround(p.y * scale))});
  }
  return out;
}

std::vector<Point> toNestCoordinates(const std::vector<Point64>& polygon, double scale) {
  std::vector<Point> out;
  out.reserve(polygon.size());
  for (const auto& p : polygon) {
    out.push_back(Point{static_cast<double>(p.x) / scale, static_cast<double>(p.y) / scale, true});
  }
  return out;
}

Path64 outerPathToClipperCoordinates(const Polygon& polygon, const Config& config) {
  std::vector<Point> outer = polygon.points;
  if (polygonArea(outer) > 0) {
    std::reverse(outer.begin(), outer.end());
  }
  return toClipperCoordinates(outer, config.clipperScale);
}

std::vector<Path64> childPathsToClipperCoordinates(const Polygon& polygon, const Config& config) {
  std::vector<Path64> children;
  for (const auto& childPoly : polygon.children) {
    std::vector<Point> child = childPoly.points;
    if (polygonArea(child) < 0) {
      std::reverse(child.begin(), child.end());
    }
    children.push_back(toClipperCoordinates(child, config.clipperScale));
  }
  return children;
}

Paths64 translatePaths(const Paths64& paths, int64_t dx, int64_t dy) {
  Paths64 translated;
  translated.reserve(paths.size());
  for (const auto& path : paths) {
    translated.push_back(translatePath(path, dx, dy));
  }
  return translated;
}

std::vector<Path64> nfpToClipperCoordinates(const Polygon& nfp, const Config& config) {
  std::vector<Path64> out;
  for (const auto& child : nfp.children) {
    std::vector<Point> childPath = child.points;
    if (polygonArea(childPath) < 0) {
      std::reverse(childPath.begin(), childPath.end());
    }
    out.push_back(toClipperCoordinates(childPath, config.clipperScale));
  }

  std::vector<Point> outer = nfp.points;
  if (polygonArea(outer) > 0) {
    std::reverse(outer.begin(), outer.end());
  }
  out.push_back(toClipperCoordinates(outer, config.clipperScale));
  return out;
}

std::vector<Path64> innerNfpToClipperCoordinates(const std::vector<Polygon>& nfp, const Config& config) {
  std::vector<Path64> out;
  for (const auto& poly : nfp) {
    auto converted = nfpToClipperCoordinates(poly, config);
    out.insert(out.end(), converted.begin(), converted.end());
  }
  return out;
}

bool hasNonZeroClipperArea(const Paths64& paths) {
  for (const auto& p : paths) {
    if (std::abs(Area(p)) > 0.0) {
      return true;
    }
  }
  return false;
}

bool hasMaterialOverlap(const Polygon& A, const Polygon& B, const Config& config) {
  Paths64 inter = Intersect(Paths64{outerPathToClipperCoordinates(A, config)},
                            Paths64{outerPathToClipperCoordinates(B, config)}, FillRule::NonZero);
  if (inter.empty()) {
    return false;
  }

  std::vector<Path64> holes = childPathsToClipperCoordinates(A, config);
  auto bholes = childPathsToClipperCoordinates(B, config);
  holes.insert(holes.end(), bholes.begin(), bholes.end());

  if (!holes.empty()) {
    inter = Difference(inter, holes, FillRule::NonZero);
  }

  return hasNonZeroClipperArea(inter);
}

bool hasMaterialOutsideSheet(const Polygon& part, const Polygon& sheet, const Config& config) {
  Paths64 outside = Difference(Paths64{outerPathToClipperCoordinates(part, config)},
                               Paths64{outerPathToClipperCoordinates(sheet, config)},
                               FillRule::NonZero);
  if (hasNonZeroClipperArea(outside)) {
    return true;
  }

  for (const auto& hole : sheet.children) {
    if (hasMaterialOverlap(part, hole, config)) {
      return true;
    }
  }

  return false;
}

MergedLengthResult mergedLength(const std::vector<Polygon>& parts, const Polygon& p, double minlength, double tolerance) {
  MergedLengthResult result;
  const double minLen2 = minlength * minlength;

  for (size_t i = 0; i < p.points.size(); ++i) {
    const auto& A1 = p.points[i];
    const auto& A2 = p.points[(i + 1) % p.points.size()];
    if (!A1.exact || !A2.exact) {
      continue;
    }

    const double ax = A2.x - A1.x;
    const double ay = A2.y - A1.y;
    if (ax * ax + ay * ay < minLen2) {
      continue;
    }

    const double angle = std::atan2(ay, ax);
    const double c = std::cos(-angle);
    const double s = std::sin(-angle);
    const double c2 = std::cos(angle);
    const double s2 = std::sin(angle);

    const double rotA2x = (A2.x - A1.x) * c - (A2.y - A1.y) * s;

    for (const auto& Bpoly : parts) {
      if (Bpoly.points.size() > 1) {
        for (size_t k = 0; k < Bpoly.points.size(); ++k) {
          const auto& B1 = Bpoly.points[k];
          const auto& B2 = Bpoly.points[(k + 1) % Bpoly.points.size()];
          if (!B1.exact || !B2.exact) {
            continue;
          }

          const double bx = B2.x - B1.x;
          const double by = B2.y - B1.y;
          if (bx * bx + by * by < minLen2) {
            continue;
          }

          const Point relB1{B1.x - A1.x, B1.y - A1.y, true};
          const Point relB2{B2.x - A1.x, B2.y - A1.y, true};

          const Point rotB1{relB1.x * c - relB1.y * s, relB1.x * s + relB1.y * c, true};
          const Point rotB2{relB2.x * c - relB2.y * s, relB2.x * s + relB2.y * c, true};

          if (!almostEqual(rotB1.y, 0.0, tolerance) || !almostEqual(rotB2.y, 0.0, tolerance)) {
            continue;
          }

          const double min1 = std::min(0.0, rotA2x);
          const double max1 = std::max(0.0, rotA2x);
          const double min2 = std::min(rotB1.x, rotB2.x);
          const double max2 = std::max(rotB1.x, rotB2.x);

          if (min2 >= max1 || max2 <= min1) {
            continue;
          }

          double len = 0.0;
          double relC1x = 0.0;
          double relC2x = 0.0;

          if (almostEqual(min1, min2) && almostEqual(max1, max2)) {
            len = max1 - min1;
            relC1x = min1;
            relC2x = max1;
          } else if (min1 > min2 && max1 < max2) {
            len = max1 - min1;
            relC1x = min1;
            relC2x = max1;
          } else if (min2 > min1 && max2 < max1) {
            len = max2 - min2;
            relC1x = min2;
            relC2x = max2;
          } else {
            len = std::max(0.0, std::min(max1, max2) - std::max(min1, min2));
            relC1x = std::min(max1, max2);
            relC2x = std::max(min1, min2);
          }

          if (len * len > minLen2) {
            result.totalLength += len;
            Point relC1{relC1x * c2, relC1x * s2, true};
            Point relC2{relC2x * c2, relC2x * s2, true};
            result.segments.push_back(Segment{{relC1.x + A1.x, relC1.y + A1.y, true},
                                              {relC2.x + A1.x, relC2.y + A1.y, true}});
          }
        }
      }

      if (!Bpoly.children.empty()) {
        auto child = mergedLength(Bpoly.children, p, minlength, tolerance);
        result.totalLength += child.totalLength;
        result.segments.insert(result.segments.end(), child.segments.begin(), child.segments.end());
      }
    }
  }

  return result;
}

}  // namespace deepnest
