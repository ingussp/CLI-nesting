#include "deepnestcpp/demo_setup.hpp"

#include <stdexcept>

namespace deepnest {

namespace {

constexpr const char* kDemoPartSource = "neregulara_zvaigzne";
constexpr const char* kDemoPartFilename = "neregulara_zvaigzne.svg";
constexpr const char* kDemoPartGeometryKey = "svg:neregulara_zvaigzne:normalized-after-transform";

Polygon makeRect(double x, double y, double w, double h, const std::string& source, std::optional<int> id) {
  Polygon p;
  p.points = {{x, y, true}, {x + w, y, true}, {x + w, y + h, true}, {x, y + h, true}};
  p.source = source;
  p.id = id;
  return p;
}

}  // namespace

Polygon makeDemoStarPolygon() {
  Polygon star;
  star.points = {
      {0.0, 0.0, true},
      {-41.522797, -25.039257, true},
      {16.091009, -35.400326, true},
      {44.61285, -63.733101, true},
      {41.736702, -24.234043, true},
      {78.924919, 28.408421000000004, true},
      {27.722533999999996, 9.815371000000003, true},
      {-12.610082000000002, 49.57589, true},
  };
  star.source = kDemoPartSource;
  star.filename = kDemoPartFilename;
  star.geometryKey = kDemoPartGeometryKey;
  return star;
}

std::vector<Polygon> makeDemoStarParts(int count) {
  if (count <= 0) {
    throw std::invalid_argument("count must be positive");
  }

  Polygon base = makeDemoStarPolygon();
  std::vector<Polygon> parts;
  parts.reserve(static_cast<size_t>(count));
  for (int id = 1; id <= count; ++id) {
    Polygon part = base;
    part.id = id;
    parts.push_back(std::move(part));
  }
  return parts;
}

Polygon makeDemoSheet() {
  return makeRect(0.0, 0.0, kDemoSheetWidthMm, kDemoSheetHeightMm, "sheet_1500x1500", 1);
}

}  // namespace deepnest
