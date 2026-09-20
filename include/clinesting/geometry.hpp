#pragma once

#include "clinesting/model.hpp"

#include <cstdint>
#include <clipper2/clipper.h>
#include <string>

namespace clinesting {

// Compare scalar values within an absolute tolerance.
bool almostEqual(double a, double b, double tolerance = 1e-9);
// Compute the signed area enclosed by a contour.
double polygonArea(const Polygon& polygon);
// Compute the signed area enclosed by a contour.
double polygonArea(const std::vector<Point>& path);
// Compute the axis-aligned bounds of a point contour.
Bounds getPolygonBounds(const std::vector<Point>& path);
// Return a cache identity based on the full polygon geometry.
std::string polygonGeometryIdentity(const Polygon& polygon);

// Translate an outline and all its child holes.
Polygon shiftPolygon(const Polygon& p, const Point& shift);
// Rotate an outline and its holes counterclockwise about the origin.
Polygon rotatePolygon(const Polygon& polygon, double degrees);
// Copy a polygon together with its child contours and metadata.
Polygon clonePolygonWithChildren(const Polygon& polygon);
// Copy the points of a polygon contour.
std::vector<Point> clonePolygonPath(const std::vector<Point>& polygon);

// Subtract hole areas from the outer contour's area.
double polygonMaterialArea(const Polygon& polygon);
// Build the convex hull of the supplied points.
std::vector<Point> getHull(const std::vector<Point>& polygon);

// Scale floating-point points into Clipper integer coordinates.
std::vector<Clipper2Lib::Point64> toClipperCoordinates(const std::vector<Point>& polygon, double scale);
// Convert Clipper integer coordinates back to nesting units.
std::vector<Point> toNestCoordinates(const std::vector<Clipper2Lib::Point64>& polygon, double scale);

// Convert a no-fit polygon and its holes into clipping paths.
std::vector<Clipper2Lib::Path64> nfpToClipperCoordinates(const Polygon& nfp, const Config& config);
// Convert feasible inner-placement regions into clipping paths.
std::vector<Clipper2Lib::Path64> innerNfpToClipperCoordinates(const std::vector<Polygon>& nfp, const Config& config);
// Convert the outer contour using the configured integer scale.
Clipper2Lib::Path64 outerPathToClipperCoordinates(const Polygon& polygon, const Config& config);
// Convert hole contours into clipping paths.
std::vector<Clipper2Lib::Path64> childPathsToClipperCoordinates(const Polygon& polygon, const Config& config);
// Translate a collection of integer clipping paths.
Clipper2Lib::Paths64 translatePaths(const Clipper2Lib::Paths64& paths, int64_t dx, int64_t dy);

// Check whether clipping produced a region with nonzero area.
bool hasNonZeroClipperArea(const Clipper2Lib::Paths64& paths);
// Detect shared material while excluding polygon holes.
bool hasMaterialOverlap(const Polygon& A, const Polygon& B, const Config& config);
// Check stock containment and overlap with stock cutouts.
bool hasMaterialOutsideSheet(const Polygon& part, const Polygon& sheet, const Config& config);

// Check physical boundary gaps without changing the exported contours.
bool violatesPartClearance(const Polygon& a, const Polygon& b, const Config& config);
// Check the independent margins to the sheet outline and its cutouts.
bool violatesSheetClearance(const Polygon& part, const Polygon& sheet, const Config& config);

// Measure sufficiently long coincident edges between placed contours.
MergedLengthResult mergedLength(const std::vector<Polygon>& parts, const Polygon& p, double minlength, double tolerance);

}  // namespace clinesting
