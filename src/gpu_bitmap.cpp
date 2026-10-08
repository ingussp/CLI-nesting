#include "clinesting/gpu_bitmap.hpp"
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace clinesting {
namespace {
// Throw when a runtime operation reports an error or deadline expiry.
void check(cl_int error, const char* operation) {
  if(error!=CL_SUCCESS) throw std::runtime_error(std::string("OpenCL ")+operation+" failed ("+std::to_string(error)+")");
}
// Load OpenCL entry points without a link-time GPU runtime dependency.
struct Api {
#ifdef _WIN32
  HMODULE library{};
#else
  void* library{};
#endif
#define CL_FUNCTIONS(X) \
  X(clGetPlatformIDs) X(clGetDeviceIDs) X(clGetDeviceInfo) X(clCreateContext) \
  X(clReleaseContext) X(clCreateCommandQueue) X(clReleaseCommandQueue) \
  X(clCreateProgramWithSource) X(clBuildProgram) X(clGetProgramBuildInfo) X(clReleaseProgram) \
  X(clCreateKernel) X(clReleaseKernel) X(clCreateBuffer) X(clReleaseMemObject) \
  X(clSetKernelArg) X(clEnqueueNDRangeKernel) X(clEnqueueReadBuffer) X(clEnqueueWriteBuffer) X(clFinish)
#define DECLARE(name) decltype(&::name) name{};
  CL_FUNCTIONS(DECLARE)
#undef DECLARE
  // Initialize the dynamically loaded OpenCL interface.
  Api() {
#ifdef _WIN32
    library=LoadLibraryExW(L"OpenCL.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    library=dlopen("libOpenCL.so.1",RTLD_NOW|RTLD_LOCAL);
#endif
    if(!library) throw std::runtime_error("OpenCL runtime not found; install the GPU vendor's OpenCL driver");
    try {
#define LOAD(name) name=reinterpret_cast<decltype(name)>(symbol(#name));
      CL_FUNCTIONS(LOAD)
#undef LOAD
    } catch(...) { close(); throw; }
  }
  // Unload the dynamically opened OpenCL library.
  ~Api() { close(); }
  // Close the loaded OpenCL library handle.
  void close() {
#ifdef _WIN32
    if(library) FreeLibrary(library);
#else
    if(library) dlclose(library);
#endif
    library=nullptr;
  }
  // Resolve a required OpenCL function from the loaded library.
  void* symbol(const char* name) {
#ifdef _WIN32
    auto p=GetProcAddress(library,name);
#else
    auto p=dlsym(library,name);
#endif
    if(!p) throw std::runtime_error(std::string("OpenCL function missing: ")+name);
    return reinterpret_cast<void*>(p);
  }
};
// Pair an OpenCL device handle with its public description.
struct Device { cl_device_id id; GpuDeviceInfo info; bool unified; cl_uint units; };
// Read a string-valued property from an OpenCL device.
std::string deviceString(Api& api,cl_device_id id,cl_device_info key) {
  size_t size=0; check(api.clGetDeviceInfo(id,key,0,nullptr,&size),"device info size");
  std::string text(size,'\0'); check(api.clGetDeviceInfo(id,key,size,text.data(),nullptr),"device info");
  if(!text.empty() && text.back()=='\0') text.pop_back(); return text;
}
// Read a scalar-valued property from an OpenCL device.
template<class T> T deviceValue(Api& api,cl_device_id id,cl_device_info key) {
  T value{}; check(api.clGetDeviceInfo(id,key,sizeof(value),&value,nullptr),"device property"); return value;
}
// Enumerate eligible GPU devices across OpenCL platforms.
std::vector<Device> devices(Api& api) {
  cl_uint count=0;
  const auto e=api.clGetPlatformIDs(0,nullptr,&count);
  if(e==-1001) return {}; // CL_PLATFORM_NOT_FOUND_KHR
  check(e,"platform enumeration");
  if(!count) return {};
  std::vector<cl_platform_id> platforms(count);
  check(api.clGetPlatformIDs(count,platforms.data(),nullptr),"platform enumeration");
  std::vector<Device> out;
  for(auto platform:platforms) {
    cl_uint n=0; const auto error=api.clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,0,nullptr,&n);
    if(error==CL_DEVICE_NOT_FOUND) continue;
    check(error,"GPU enumeration");
    if(!n) continue;
    std::vector<cl_device_id> ids(n);
    check(api.clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,n,ids.data(),nullptr),"GPU enumeration");
    for(auto id:ids) {
      if(!deviceValue<cl_bool>(api,id,CL_DEVICE_AVAILABLE) || !deviceValue<cl_bool>(api,id,CL_DEVICE_COMPILER_AVAILABLE)) continue;
      out.push_back({id,{int(out.size()),deviceString(api,id,CL_DEVICE_NAME),deviceString(api,id,CL_DEVICE_VENDOR),
          deviceValue<cl_ulong>(api,id,CL_DEVICE_GLOBAL_MEM_SIZE)},
          bool(deviceValue<cl_bool>(api,id,CL_DEVICE_HOST_UNIFIED_MEMORY)),
          deviceValue<cl_uint>(api,id,CL_DEVICE_MAX_COMPUTE_UNITS)});
    }
  }
  return out;
}
const char* source=R"CLC(
typedef struct { uint width, height, stride, offset; } Mask;
typedef struct { int x, y; uint rotation; } Candidate;
// Check one translated mask against occupied and permitted stock pixels.
inline uchar fits(__global const ulong* occupied, __global const ulong* material,
                  __global const Mask* masks, __global const ulong* bits,
                  uint sw, uint sh, uint sheetStride, uint rotations, int x, int y, uint r) {
  if(r>=rotations || x<0 || y<0) return 0;
  Mask m=masks[r];
  if(m.width>sw || m.height>sh || (uint)x>sw-m.width || (uint)y>sh-m.height) return 0;
  uint shift=(uint)x&63, word=(uint)x>>6;
  uint needed=(m.width+shift+63)/64;
  for(uint row=0;row<m.height;++row) {
    uint dst=((uint)y+row)*sheetStride+word, src=m.offset+row*m.stride;
    for(uint w=0;w<needed;++w) {
      ulong v=w<m.stride ? bits[src+w] : 0;
      if(shift) v=(v<<shift) | (w>0 && w-1<m.stride ? bits[src+w-1]>>(64-shift) : 0);
      if((v & occupied[dst+w]) || (v & ~material[dst+w])) return 0;
    }
  }
  return 1;
}
// Filter explicit or implicit-grid candidates independently on GPU work items.
__kernel void collision(__global const ulong* occupied, __global const ulong* material,
                        __global const Mask* masks, __global const ulong* bits,
                        __global const Candidate* candidates, __global uchar* output,
                        uint sw,uint sh,uint stride,uint rotations,uint count,
                        uint mode,ulong first,uint rows,uint step,uint ww,uint wh,uint firstMask,uint maskCount,uint originX,uint originY) {
  uint i=get_global_id(0); if(i>=count) return;
  int x,y; uint r;
  if(mode) {
    ulong p=first+(ulong)i;
    r=firstMask+(uint)(p%maskCount); p/=maskCount;
    y=(int)(p%rows)*step+originY; x=(int)(p/rows)*step+originX;
  } else { Candidate c=candidates[i]; x=c.x; y=c.y; r=c.rotation; }
  if(r>=rotations || x<0 || y<0) { output[i]=0; return; }
  Mask m=masks[r];
  if(m.width>ww || m.height>wh || (uint)x>ww-m.width || (uint)y>wh-m.height) { output[i]=0; return; }
  output[i]=fits(occupied,material,masks,bits,sw,sh,stride,rotations,x,y,r);
}
// One work item owns one destination word, so no atomic operations are needed.
__kernel void toggle_mask(__global ulong* occupied,__global const Mask* masks,
                          __global const ulong* bits,uint stride,uint rotation,int x,int y) {
  Mask m=masks[rotation];uint shift=(uint)x&63,word=(uint)x>>6;
  uint needed=(m.width+shift+63)/64;
  uint index=get_global_id(0),row=index/needed,w=index%needed;
  if(row>=m.height) return;
  uint src=m.offset+row*m.stride;
  ulong value=w<m.stride ? bits[src+w] : 0;
  if(shift) value=(value<<shift)|(w && w-1<m.stride ? bits[src+w-1]>>(64-shift) : 0);
  occupied[((uint)y+row)*stride+word+w]^=value;
}
)CLC";
}

// Store OpenCL context, kernels and buffers behind the public interface.
struct GpuBitmap::Impl {
  Api api;
  GpuDeviceInfo info;
  cl_device_id id{};
  cl_context context{};
  cl_command_queue queue{};
  cl_program program{};
  cl_kernel kernel{},toggleKernel{};
  std::vector<GpuMaskInfo> maskInfo;
  std::vector<uint8_t> maskReady;
  cl_mem material{},occupancy{},masks{},bits{},candidates{},output{};
  size_t capacity=0, sheetBytes=0;
  bool occupancyReady=false;
  uint64_t occupancySlot=0;
  struct SavedOccupancy {cl_mem memory;bool ready;};
  std::unordered_map<uint64_t,SavedOccupancy> savedOccupancies;
  void clearSavedOccupancies() {
    for(const auto& [slot,saved]:savedOccupancies) api.clReleaseMemObject(saved.memory);
    savedOccupancies.clear();
  }
  cl_ulong maxAllocation=0;
  cl_uint width=0,height=0,stride=0,rotations=0;
  // Release OpenCL buffers, kernels, queues and context.
  ~Impl() {
    if(queue) api.clFinish(queue);
    clearSavedOccupancies();
    for(auto m:{material,occupancy,masks,bits,candidates,output}) if(m) api.clReleaseMemObject(m);
    if(kernel) api.clReleaseKernel(kernel);
    if(toggleKernel) api.clReleaseKernel(toggleKernel);
    if(program) api.clReleaseProgram(program);
    if(queue) api.clReleaseCommandQueue(queue);
    if(context) api.clReleaseContext(context);
  }
  // Create the selected device's context, queue and collision kernels.
  void initialize(int index) {
    const auto list=devices(api);
    if(list.empty()) throw std::runtime_error("No available OpenCL GPU found");
    int selected=index;
    if(index==-1) {
      selected=0;
      for(size_t i=1;i<list.size();++i)
        if(std::pair{!list[i].unified,list[i].units}>std::pair{!list[size_t(selected)].unified,list[size_t(selected)].units}) selected=int(i);
    }
    if(selected<0 || size_t(selected)>=list.size()) throw std::runtime_error("OpenCL GPU device index is out of range; use --list-gpus");
    id=list[size_t(selected)].id; info=list[size_t(selected)].info;
    maxAllocation=deviceValue<cl_ulong>(api,id,CL_DEVICE_MAX_MEM_ALLOC_SIZE);
    cl_int e=0;
    context=api.clCreateContext(nullptr,1,&id,nullptr,nullptr,&e); check(e,"create context");
    queue=api.clCreateCommandQueue(context,id,0,&e); check(e,"create queue");
    const size_t length=std::strlen(source);
    program=api.clCreateProgramWithSource(context,1,&source,&length,&e); check(e,"create program");
    e=api.clBuildProgram(program,1,&id,"-cl-std=CL1.2",nullptr,nullptr);
    if(e!=CL_SUCCESS) {
      size_t size=0; api.clGetProgramBuildInfo(program,id,CL_PROGRAM_BUILD_LOG,0,nullptr,&size);
      std::string log(size,'\0'); if(size) api.clGetProgramBuildInfo(program,id,CL_PROGRAM_BUILD_LOG,size,log.data(),nullptr);
      throw std::runtime_error("OpenCL kernel compilation failed: "+log);
    }
    kernel=api.clCreateKernel(program,"collision",&e); check(e,"create kernel");
    toggleKernel=api.clCreateKernel(program,"toggle_mask",&e); check(e,"create toggle kernel");
  }
  // Replace a GPU buffer with data sized for the next upload.
  void replace(cl_mem& memory,size_t bytes,cl_mem_flags flags,const void* data=nullptr) {
    if(!bytes || bytes>maxAllocation || bytes>512ULL*1024*1024)
      throw std::runtime_error("OpenCL buffer exceeds the GPU allocation limit (maximum 512 MiB per buffer)");
    if(memory) { api.clReleaseMemObject(memory); memory=nullptr; }
    cl_int e=0; memory=api.clCreateBuffer(context,flags|(data?CL_MEM_COPY_HOST_PTR:0),bytes,const_cast<void*>(data),&e);
    check(e,"allocate buffer");
  }
  // Assign a checked argument to the active OpenCL kernel.
  template<class T> void arg(cl_uint index,const T& value) { check(api.clSetKernelArg(kernel,index,sizeof(value),&value),"kernel argument"); }
  // Execute a collision kernel and read back candidate validity flags.
  std::vector<uint8_t> run(uint32_t count,uint32_t mode,uint64_t first,uint32_t rows,uint32_t step,uint32_t ww,uint32_t wh,
                         std::span<const GpuCandidate> input={},uint32_t firstMask=0,uint32_t maskCount=0,uint32_t originX=0,uint32_t originY=0) {
    if(!count) return {};
    if(count>262144 || !material || !occupancyReady || !masks || !bits || !rows || !step)
      throw std::invalid_argument("Invalid OpenCL collision batch");
    if(count>capacity) {
      replace(candidates,size_t(count)*sizeof(GpuCandidate),CL_MEM_READ_ONLY);
      replace(output,count,CL_MEM_WRITE_ONLY);
      capacity=count;
    }
    if(!input.empty()) check(api.clEnqueueWriteBuffer(queue,candidates,CL_TRUE,0,input.size_bytes(),input.data(),0,nullptr,nullptr),"upload candidates");
    arg(0,occupancy); arg(1,material); arg(2,masks); arg(3,bits); arg(4,candidates); arg(5,output);
    arg(6,width); arg(7,height); arg(8,stride); arg(9,rotations); arg(10,count); arg(11,mode);
    const cl_ulong start=first; arg(12,start); arg(13,rows); arg(14,step); arg(15,ww); arg(16,wh);
    if(!maskCount) maskCount=rotations;
    arg(17,firstMask); arg(18,maskCount); arg(19,originX); arg(20,originY);
    const size_t global=count;
    check(api.clEnqueueNDRangeKernel(queue,kernel,1,nullptr,&global,nullptr,0,nullptr,nullptr),"dispatch collision kernel");
    std::vector<uint8_t> flags(count);
    check(api.clEnqueueReadBuffer(queue,output,CL_TRUE,0,flags.size(),flags.data(),0,nullptr,nullptr),"read collision results");
    return flags;
  }
};
// Return public descriptions of available OpenCL GPUs.
std::vector<GpuDeviceInfo> listGpuDevices() {
  Api api; std::vector<GpuDeviceInfo> out; for(const auto& d:devices(api)) out.push_back(d.info); return out;
}
// Initialize GPU collision filtering for the selected device.
GpuBitmap::GpuBitmap(int index) : impl_(std::make_unique<Impl>()) { impl_->initialize(index); }
// Release GPU filtering resources through the implementation owner.
GpuBitmap::~GpuBitmap()=default;
// Return the selected GPU's public device information.
const GpuDeviceInfo& GpuBitmap::device() const { return impl_->info; }
// Upload the permitted stock-material bitmap.
void GpuBitmap::setSheet(uint32_t w,uint32_t h,std::span<const uint64_t> material) {
  const uint64_t stride=(uint64_t(w)+63)/64;
  if(!w || !h || material.size()!=stride*h || material.size()>UINT32_MAX) throw std::invalid_argument("Invalid GPU sheet dimensions");
  impl_->clearSavedOccupancies();impl_->occupancySlot=0;
  impl_->width=w; impl_->height=h; impl_->stride=uint32_t(stride); impl_->sheetBytes=material.size_bytes();
  impl_->occupancyReady=false;
  impl_->replace(impl_->material,material.size_bytes(),CL_MEM_READ_ONLY,material.data());
  impl_->replace(impl_->occupancy,material.size_bytes(),CL_MEM_READ_WRITE);
}
// Upload material already occupied by accepted parts.
void GpuBitmap::setOccupancy(std::span<const uint64_t> occupancy) {
  if(occupancy.size_bytes()!=impl_->sheetBytes || !impl_->occupancy) throw std::invalid_argument("Invalid GPU occupancy size");
  check(impl_->api.clEnqueueWriteBuffer(impl_->queue,impl_->occupancy,CL_TRUE,0,occupancy.size_bytes(),occupancy.data(),0,nullptr,nullptr),"upload occupancy");
  impl_->occupancyReady=true;
}
// Retain occupancies across worker switches, bounded to 128 MiB (or 1/16 VRAM).
// The active bitmap is always available even when a sheet exceeds that budget.
bool GpuBitmap::selectOccupancySlot(uint64_t slot) {
  auto& p=*impl_;
  if(!p.occupancy || !p.sheetBytes) throw std::invalid_argument("Set GPU sheet before selecting occupancy");
  if(slot==p.occupancySlot) return p.occupancyReady;
  cl_mem next=nullptr;bool ready=false;
  const auto found=p.savedOccupancies.find(slot);
  if(found!=p.savedOccupancies.end()) {
    next=found->second.memory;ready=found->second.ready;p.savedOccupancies.erase(found);
  }
  const size_t limit=size_t(std::max<uint64_t>(1,std::min<uint64_t>(128ULL*1024*1024,p.info.memoryBytes/16)/p.sheetBytes));
  while(p.savedOccupancies.size()+1>=limit && !p.savedOccupancies.empty()) {
    auto victim=p.savedOccupancies.begin();p.api.clReleaseMemObject(victim->second.memory);p.savedOccupancies.erase(victim);
  }
  if(limit>1) p.savedOccupancies.emplace(p.occupancySlot,Impl::SavedOccupancy{p.occupancy,p.occupancyReady});
  else p.api.clReleaseMemObject(p.occupancy);
  p.occupancy=next;p.occupancyReady=ready;p.occupancySlot=slot;
  if(!p.occupancy) p.replace(p.occupancy,p.sheetBytes,CL_MEM_READ_WRITE);
  return ready;
}
// Allocate the atlas separately from per-policy pixel uploads.
void GpuBitmap::allocateMasks(std::span<const GpuMaskInfo> masks,size_t wordCount) {
  if(masks.empty() || masks.size()>UINT32_MAX || !wordCount || wordCount>UINT32_MAX)
    throw std::invalid_argument("Invalid GPU masks");
  for(const auto& m:masks) if(!m.width || !m.height || m.wordsPerRow!=(uint64_t(m.width)+63)/64 ||
      uint64_t(m.offset)+uint64_t(m.height)*m.wordsPerRow>wordCount) throw std::invalid_argument("Invalid GPU mask bounds");
  impl_->replace(impl_->masks,masks.size_bytes(),CL_MEM_READ_ONLY,masks.data());
  impl_->replace(impl_->bits,wordCount*sizeof(uint64_t),CL_MEM_READ_ONLY);
  impl_->rotations=cl_uint(masks.size());
  impl_->maskInfo.assign(masks.begin(),masks.end());
  impl_->maskReady.assign(masks.size(),0);
}
void GpuBitmap::uploadMaskRange(uint32_t first,uint32_t count,std::span<const uint64_t> words) {
  const auto& info=impl_->maskInfo;
  if(!count || first>=info.size() || count>info.size()-first) throw std::invalid_argument("Invalid GPU upload range");
  const size_t offset=info[first].offset;
  size_t end=offset;
  for(size_t i=first;i<size_t(first)+count;++i) {
    if(info[i].offset!=end) throw std::invalid_argument("GPU upload masks must be contiguous");
    end+=size_t(info[i].height)*info[i].wordsPerRow;
  }
  if(words.size()!=end-offset) throw std::invalid_argument("Invalid GPU upload size");
  check(impl_->api.clEnqueueWriteBuffer(impl_->queue,impl_->bits,CL_TRUE,offset*sizeof(uint64_t),
      words.size_bytes(),words.data(),0,nullptr,nullptr),"upload mask range");
  std::fill(impl_->maskReady.begin()+first,impl_->maskReady.begin()+first+count,1);
}
// Existing callers upload the complete atlas in one operation.
void GpuBitmap::setMasks(std::span<const GpuMaskInfo> masks,std::span<const uint64_t> words) {
  allocateMasks(masks,words.size());
  check(impl_->api.clEnqueueWriteBuffer(impl_->queue,impl_->bits,CL_TRUE,0,words.size_bytes(),
      words.data(),0,nullptr,nullptr),"upload masks");
  std::fill(impl_->maskReady.begin(),impl_->maskReady.end(),1);
}
// Evaluate a batch of explicit candidate placements on the GPU.
std::vector<uint8_t> GpuBitmap::filter(std::span<const GpuCandidate> candidates) {
  for(const auto& c:candidates) if(c.rotation<impl_->maskReady.size() && !impl_->maskReady[c.rotation])
    throw std::invalid_argument("GPU mask pixels are not uploaded");
  if(candidates.size()>262144) throw std::invalid_argument("GPU batch is too large");
  return impl_->run(uint32_t(candidates.size()),0,0,1,1,impl_->width,impl_->height,candidates);
}
// Evaluate a contiguous range of an implicit candidate grid.
std::vector<uint8_t> GpuBitmap::filterGrid(uint64_t first,uint32_t count,uint32_t rows,uint32_t step,uint32_t ww,uint32_t wh,uint32_t firstMask,uint32_t maskCount,uint32_t originX,uint32_t originY) {
  if(!maskCount) maskCount=impl_->rotations;
  if(firstMask>impl_->rotations || maskCount>impl_->rotations-firstMask) throw std::invalid_argument("Invalid GPU mask range");
  if(std::find(impl_->maskReady.begin()+firstMask,impl_->maskReady.begin()+firstMask+maskCount,uint8_t(0))!=impl_->maskReady.begin()+firstMask+maskCount)
    throw std::invalid_argument("GPU mask pixels are not uploaded");
  if(!rows || !step || !impl_->rotations || uint64_t(rows)*step>uint64_t(INT32_MAX) ||
      first>UINT64_MAX-count || (first+count)/maskCount/rows>uint64_t(INT32_MAX)/step)
    throw std::invalid_argument("Invalid GPU grid range");
  if(originX>uint32_t(INT32_MAX)||originY>uint32_t(INT32_MAX) ||
      (first+count)/maskCount/rows*step+originX>uint64_t(INT32_MAX) || uint64_t(rows)*step+originY>uint64_t(INT32_MAX))
    throw std::invalid_argument("Invalid GPU grid origin");
  return impl_->run(count,1,first,rows,step,ww,wh,{},firstMask,maskCount,originX,originY);
}
}

namespace clinesting {
void GpuBitmap::toggleMask(uint32_t rotation,int32_t x,int32_t y) {
  auto& p=*impl_;
  if(!p.occupancyReady || rotation>=p.maskInfo.size() || !p.maskReady[rotation] || x<0 || y<0)
    throw std::invalid_argument("Invalid GPU occupancy update");
  const auto& mask=p.maskInfo[rotation];
  if(uint64_t(x)+mask.width>p.width || uint64_t(y)+mask.height>p.height)
    throw std::invalid_argument("GPU occupancy update outside stock");
  auto arg=[&](cl_uint index,const auto& value) {
    check(p.api.clSetKernelArg(p.toggleKernel,index,sizeof(value),&value),"toggle argument");
  };
  arg(0,p.occupancy);arg(1,p.masks);arg(2,p.bits);arg(3,p.stride);
  arg(4,rotation);arg(5,x);arg(6,y);
  const size_t count=size_t(mask.height)*((mask.width+(uint32_t(x)&63)+63)/64);
  check(p.api.clEnqueueNDRangeKernel(p.queue,p.toggleKernel,1,nullptr,&count,nullptr,0,nullptr,nullptr),"toggle occupancy");
  // The in-order queue ensures subsequent filtering sees this update. No readback.
}
}
