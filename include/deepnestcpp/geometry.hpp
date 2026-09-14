#pragma once

#include "deepnestcpp/model.hpp"

#include <cstdint>
#include <clipper2/clipper.h>
#include <string>

namespace deepnest {

bool almostEqual(double a, double b, double tolerance = 1e-9);
double polygonArea(const Polygon& polygon);
double polygonArea(const std::vector<Point>& path);
Bounds getPolygonBounds(const std::vector<Point>& path);
std::string polygonGeometryIdentity(const Polygon& polygon);

Polygon shiftPolygon(const Polygon& p, const Point& shift);
Polygon rotatePolygon(const Polygon& polygon, double degrees);
Polygon clonePolygonWithChildren(const Polygon& polygon);
std::vector<Point> clonePolygonPath(const std::vector<Point>& polygon);

double polygonMaterialArea(const Polygon& polygon);
std::vector<Point> getHull(const std::vector<Point>& polygon);

std::vector<Clipper2Lib::Point64> toClipperCoordinates(const std::vector<Point>& polygon, double scale);
std::vector<Point> toNestCoordinates(const std::vector<Clipper2Lib::Point64>& polygon, double scale);

std::vector<Clipper2Lib::Path64> nfpToClipperCoordinates(const Polygon& nfp, const Config& config);
std::vector<Clipper2Lib::Path64> innerNfpToClipperCoordinates(const std::vector<Polygon>& nfp, const Config& config);
Clipper2Lib::Path64 outerPathToClipperCoordinates(const Polygon& polygon, const Config& config);
std::vector<Clipper2Lib::Path64> childPathsToClipperCoordinates(const Polygon& polygon, const Config& config);
Clipper2Lib::Paths64 translatePaths(const Clipper2Lib::Paths64& paths, int64_t dx, int64_t dy);

bool hasNonZeroClipperArea(const Clipper2Lib::Paths64& paths);
bool hasMaterialOverlap(const Polygon& A, const Polygon& B, const Config& config);
bool hasMaterialOutsideSheet(const Polygon& part, const Polygon& sheet, const Config& config);

MergedLengthResult mergedLength(const std::vector<Polygon>& parts, const Polygon& p, double minlength, double tolerance);

}  // namespace deepnest
