#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace deepnest {
struct GpuDeviceInfo {
  int index;
  std::string name, vendor;
  uint64_t memoryBytes;
};
std::vector<GpuDeviceInfo> listGpuDevices();

// OpenCL receives the same integer raster words used by the CPU. No floating
// point geometry or layout decisions are delegated to the collision kernel.
struct GpuMaskInfo {
  uint32_t width, height, wordsPerRow, offset;
};
struct GpuCandidate { int32_t x, y; uint32_t rotation; };

class GpuBitmap {
 public:
  explicit GpuBitmap(int deviceIndex = -1);
  ~GpuBitmap();
  GpuBitmap(const GpuBitmap&) = delete;
  GpuBitmap& operator=(const GpuBitmap&) = delete;
  const GpuDeviceInfo& device() const;
  void setSheet(uint32_t width, uint32_t height, std::span<const uint64_t> material);
  void setOccupancy(std::span<const uint64_t> occupancy);
  void setMasks(std::span<const GpuMaskInfo> masks, std::span<const uint64_t> words);
  // Flags preserve input order, including invalid origins. A 1 means raster
  // feasible; callers must still perform exact geometry validation.
  std::vector<uint8_t> filter(std::span<const GpuCandidate> candidates);
  std::vector<uint8_t> filterGrid(uint64_t first, uint32_t count, uint32_t rows,
                                uint32_t step, uint32_t windowWidth, uint32_t windowHeight);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
