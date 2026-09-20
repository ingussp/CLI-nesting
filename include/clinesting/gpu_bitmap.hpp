#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace clinesting {
// Describe an available OpenCL GPU and its memory capacity.
struct GpuDeviceInfo {
  int index;
  std::string name, vendor;
  uint64_t memoryBytes;
};
// Return public descriptions of available OpenCL GPUs.
std::vector<GpuDeviceInfo> listGpuDevices();

// OpenCL receives the same integer raster words used by the CPU. No floating
// point geometry or layout decisions are delegated to the collision kernel.
struct GpuMaskInfo {
  uint32_t width, height, wordsPerRow, offset;
};
// Identify one mask and candidate translation for GPU filtering.
struct GpuCandidate { int32_t x, y; uint32_t rotation; };

// Own OpenCL resources for batched bitmap collision filtering.
class GpuBitmap {
 public:
  // Own OpenCL resources for batched bitmap collision filtering.
  explicit GpuBitmap(int deviceIndex = -1);
  // Release GPU filtering resources through the implementation owner.
  ~GpuBitmap();
  // Initialize GPU collision filtering for the selected device.
  GpuBitmap(const GpuBitmap&) = delete;
  // Define assignment behavior for this resource-owning object.
  GpuBitmap& operator=(const GpuBitmap&) = delete;
  // Return the selected GPU's public device information.
  const GpuDeviceInfo& device() const;
  // Upload the permitted stock-material bitmap.
  void setSheet(uint32_t width, uint32_t height, std::span<const uint64_t> material);
  // Upload material already occupied by accepted parts.
  void setOccupancy(std::span<const uint64_t> occupancy);
  // Upload packed rotation masks and their dimensions.
  void setMasks(std::span<const GpuMaskInfo> masks, std::span<const uint64_t> words);
  // Flags preserve input order, including invalid origins. A 1 means raster
  // feasible; callers must still perform exact geometry validation.
  std::vector<uint8_t> filter(std::span<const GpuCandidate> candidates);
  // Evaluate a contiguous range of an implicit candidate grid.
  std::vector<uint8_t> filterGrid(uint64_t first, uint32_t count, uint32_t rows,
                                uint32_t step, uint32_t windowWidth, uint32_t windowHeight);
 private:
  // Store OpenCL context, kernels and buffers behind the public interface.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
