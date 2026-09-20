#include "clinesting/dxf_export.hpp"

#include "clinesting/geometry.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace clinesting {

namespace {

using IndexBuckets = std::unordered_map<std::string, std::vector<size_t>>;

// Read the optional physical instance identifier from a placement.
const std::optional<int>& placementId(const Placement& placement) {
  return placement.id;
}

// Read the optional physical instance identifier from a placement.
const std::optional<int>& placementId(const SheetPlacement& placement) {
  return placement.sheetid;
}

// Read the source geometry identifier from a placement.
const std::string& placementSource(const Placement& placement) {
  return placement.source;
}

// Read the source geometry identifier from a placement.
const std::string& placementSource(const SheetPlacement& placement) {
  return placement.sheet;
}

// Combine source and instance identifiers for export lookup.
std::string makeSourceIdKey(const std::string& source, const std::optional<int>& id) {
  return source + '\x1F' + (id.has_value() ? std::to_string(*id) : std::string("null"));
}

// Index input geometry by source and optional instance ID.
void indexBySourceAndId(const std::vector<Polygon>& polygons,
                        IndexBuckets& bySourceAndId,
                        IndexBuckets& byId,
                        IndexBuckets& bySource) {
  for (size_t i = 0; i < polygons.size(); ++i) {
    bySourceAndId[makeSourceIdKey(polygons[i].source, polygons[i].id)].push_back(i);
    bySource[polygons[i].source].push_back(i);
    if (polygons[i].id.has_value()) {
      byId[std::to_string(*polygons[i].id)].push_back(i);
    }
  }
}

// Resolve an unused matching input entry for an output placement.
std::optional<size_t> consumeIndex(const std::vector<size_t>& bucket,
                                   std::vector<bool>& consumed,
                                   std::unordered_map<std::string, size_t>& cursors,
                                   const std::string& key) {
  size_t& cursor = cursors[key];
  while (cursor < bucket.size() && consumed[bucket[cursor]]) {
    ++cursor;
  }
  if (cursor >= bucket.size()) {
    return std::nullopt;
  }
  const size_t idx = bucket[cursor];
  consumed[idx] = true;
  ++cursor;
  return idx;
}

// Resolve placement identities to original input geometry for DXF.
class PlacementResolver {
 public:
  // Initialize source/instance lookup for DXF geometry.
  explicit PlacementResolver(const std::vector<Polygon>& polygons) : polygons_(polygons), consumed_(polygons.size(), false) {
    indexBySourceAndId(polygons_, bySourceAndId_, byId_, bySource_);
  }

  // Resolve an exported placement to the corresponding input contour.
  template <typename PlacementLike>
  const Polygon& resolve(const PlacementLike& placement, const std::string& kind) {
    std::optional<size_t> resolved;
    const auto& placementIdValue = placementId(placement);
    const auto& placementSourceValue = placementSource(placement);

    if (placementIdValue.has_value()) {
      const std::string idKey = std::to_string(*placementIdValue);
      if (!placementSourceValue.empty()) {
        const std::string sourceIdKey = makeSourceIdKey(placementSourceValue, placementIdValue);
        auto it = bySourceAndId_.find(sourceIdKey);
        if (it != bySourceAndId_.end()) {
          resolved = consumeIndex(it->second, consumed_, cursors_, "sid:" + sourceIdKey);
        }
      }

      if (!resolved.has_value()) {
        auto idIt = byId_.find(idKey);
        if (idIt != byId_.end()) {
          resolved = consumeIndex(idIt->second, consumed_, cursors_, "id:" + idKey);
        }
      }
    }

    if (!resolved.has_value()) {
      const std::string sourceKey = placementSourceValue;
      auto sourceIt = bySource_.find(sourceKey);
      if (sourceIt != bySource_.end()) {
        resolved = consumeIndex(sourceIt->second, consumed_, cursors_, "src:" + sourceKey);
      }
    }

    if (!resolved.has_value()) {
      std::ostringstream oss;
      oss << "Unable to resolve " << kind << " placement for source='" << placementSourceValue << "'";
      if (placementIdValue.has_value()) {
        oss << " id=" << *placementIdValue;
      }
      throw std::runtime_error(oss.str());
    }

    return polygons_[*resolved];
  }

 private:
  const std::vector<Polygon>& polygons_;
  IndexBuckets bySourceAndId_;
  IndexBuckets byId_;
  IndexBuckets bySource_;
  std::vector<bool> consumed_;
  std::unordered_map<std::string, size_t> cursors_;
};

// Format a coordinate without locale-specific DXF syntax.
std::string formatDouble(double value) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(6) << value;
  return oss.str();
}

// Write one DXF group-code and value pair.
void writePair(std::ostream& out, int code, const std::string& value) {
  out << code << "\n" << value << "\n";
}

// Emit a named layer in the DXF layer table.
void writeLayer(std::ostream& out, const std::string& layerName, int color) {
  writePair(out, 0, "LAYER");
  writePair(out, 2, layerName);
  writePair(out, 70, "0");
  writePair(out, 62, std::to_string(color));
  writePair(out, 6, "CONTINUOUS");
}

// Normalize a contour before writing its DXF polyline.
std::vector<Point> normalizedPolyline(const std::vector<Point>& points) {
  if (points.empty()) {
    return {};
  }
  std::vector<Point> out = points;
  if (out.size() > 1 && almostEqual(out.front().x, out.back().x) && almostEqual(out.front().y, out.back().y)) {
    out.pop_back();
  }
  return out;
}

// Write a closed DXF polyline from contour points.
void writePolyline(std::ostream& out, const std::vector<Point>& points, const std::string& layerName) {
  const auto normalized = normalizedPolyline(points);
  if (normalized.size() < 2) {
    return;
  }

  writePair(out, 0, "LWPOLYLINE");
  writePair(out, 8, layerName);
  writePair(out, 90, std::to_string(normalized.size()));
  writePair(out, 70, "1");
  for (const auto& pt : normalized) {
    writePair(out, 10, formatDouble(pt.x));
    writePair(out, 20, formatDouble(pt.y));
  }
}

// Write child contours as hole polylines.
void writeHolesRecursive(std::ostream& out, const Polygon& polygon) {
  for (const auto& child : polygon.children) {
    writePolyline(out, child.points, "HOLES");
    writeHolesRecursive(out, child);
  }
}

// Write a text label at the requested DXF position.
void writeLabel(std::ostream& out, const Polygon& polygon, const std::string& text, double height) {
  if (text.empty() || polygon.points.empty()) {
    return;
  }
  const auto bounds = getPolygonBounds(polygon.points);
  const double x = bounds.x + bounds.width / 2.0;
  const double y = bounds.y + bounds.height / 2.0;

  writePair(out, 0, "TEXT");
  writePair(out, 8, "LABELS");
  writePair(out, 10, formatDouble(x));
  writePair(out, 20, formatDouble(y));
  writePair(out, 40, formatDouble(height));
  writePair(out, 1, text);
}

// Write DXF headers, tables and the start of the entity section.
void writeDxfPrefix(std::ostream& out) {
  writePair(out, 0, "SECTION");
  writePair(out, 2, "HEADER");
  writePair(out, 0, "ENDSEC");

  writePair(out, 0, "SECTION");
  writePair(out, 2, "TABLES");
  writePair(out, 0, "TABLE");
  writePair(out, 2, "LAYER");
  writePair(out, 70, "4");
  writeLayer(out, "SHEETS", 7);
  writeLayer(out, "PARTS", 1);
  writeLayer(out, "HOLES", 5);
  writeLayer(out, "LABELS", 3);
  writePair(out, 0, "ENDTAB");
  writePair(out, 0, "ENDSEC");

  writePair(out, 0, "SECTION");
  writePair(out, 2, "ENTITIES");
}

// Close the DXF entity section and file.
void writeDxfSuffix(std::ostream& out) {
  writePair(out, 0, "ENDSEC");
  writePair(out, 0, "EOF");
}

}  // namespace

// Write placed parts and stock as genuine DXF entities.
void exportPlacementResultToDxf(const std::filesystem::path& outputPath,
                                const std::vector<Polygon>& sourceSheets,
                                const std::vector<Polygon>& sourceParts,
                                const PlacementResult& result,
                                const DxfExportOptions& options) {
  if (sourceSheets.empty()) {
    throw std::runtime_error("DXF export requires at least one source sheet polygon");
  }
  if (sourceParts.empty()) {
    throw std::runtime_error("DXF export requires at least one source part polygon");
  }

  std::ofstream out(outputPath, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    throw std::runtime_error("Unable to open DXF output path: " + outputPath.string());
  }

  writeDxfPrefix(out);
  PlacementResolver sheetResolver(sourceSheets);
  PlacementResolver partResolver(sourceParts);

  for (const auto& placedSheet : result.placements) {
    const auto& sourceSheet = sheetResolver.resolve(placedSheet, "sheet");
    writePolyline(out, sourceSheet.points, "SHEETS");
    writeHolesRecursive(out, sourceSheet);

    for (const auto& partPlacement : placedSheet.sheetplacements) {
      const auto& sourcePart = partResolver.resolve(partPlacement, "part");
      Polygon transformed = rotatePolygon(sourcePart, partPlacement.rotation);
      transformed = shiftPolygon(transformed, {partPlacement.x, partPlacement.y, true});

      writePolyline(out, transformed.points, "PARTS");
      writeHolesRecursive(out, transformed);

      if (options.includeLabels) {
        const std::string label =
            !partPlacement.source.empty()
                ? partPlacement.source
                : (partPlacement.id.has_value() ? std::to_string(*partPlacement.id) : std::string("part"));
        writeLabel(out, transformed, label, std::max(options.labelHeight, 0.001));
      }
    }
  }

  writeDxfSuffix(out);
}

}  // namespace clinesting
