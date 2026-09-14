#include "deepnestcpp/gpu_bitmap.hpp"
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace deepnest {
namespace {
void check(cl_int error, const char* operation) {
  if(error!=CL_SUCCESS) throw std::runtime_error(std::string("OpenCL ")+operation+" failed ("+std::to_string(error)+")");
}
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
  ~Api() { close(); }
  void close() {
#ifdef _WIN32
    if(library) FreeLibrary(library);
#else
    if(library) dlclose(library);
#endif
    library=nullptr;
  }
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
struct Device { cl_device_id id; GpuDeviceInfo info; bool unified; cl_uint units; };
std::string deviceString(Api& api,cl_device_id id,cl_device_info key) {
  size_t size=0; check(api.clGetDeviceInfo(id,key,0,nullptr,&size),"device info size");
  std::string text(size,'\0'); check(api.clGetDeviceInfo(id,key,size,text.data(),nullptr),"device info");
  if(!text.empty() && text.back()=='\0') text.pop_back(); return text;
}
template<class T> T deviceValue(Api& api,cl_device_id id,cl_device_info key) {
  T value{}; check(api.clGetDeviceInfo(id,key,sizeof(value),&value,nullptr),"device property"); return value;
}
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
__kernel void collision(__global const ulong* occupied, __global const ulong* material,
                        __global const Mask* masks, __global const ulong* bits,
                        __global const Candidate* candidates, __global uchar* output,
                        uint sw,uint sh,uint stride,uint rotations,uint count,
                        uint mode,ulong first,uint rows,uint step,uint ww,uint wh) {
  uint i=get_global_id(0); if(i>=count) return;
  int x,y; uint r;
  if(mode) {
    ulong p=first+(ulong)i;
    r=(uint)(p%rotations); p/=rotations;
    y=(int)(p%rows)*step; x=(int)(p/rows)*step;
  } else { Candidate c=candidates[i]; x=c.x; y=c.y; r=c.rotation; }
  if(r>=rotations || x<0 || y<0) { output[i]=0; return; }
  Mask m=masks[r];
  if(m.width>ww || m.height>wh || (uint)x>ww-m.width || (uint)y>wh-m.height) { output[i]=0; return; }
  output[i]=fits(occupied,material,masks,bits,sw,sh,stride,rotations,x,y,r);
}
)CLC";
}

struct GpuBitmap::Impl {
  Api api;
  GpuDeviceInfo info;
  cl_device_id id{};
  cl_context context{};
  cl_command_queue queue{};
  cl_program program{};
  cl_kernel kernel{};
  cl_mem material{},occupancy{},masks{},bits{},candidates{},output{};
  size_t capacity=0, sheetBytes=0;
  bool occupancyReady=false;
  cl_ulong maxAllocation=0;
  cl_uint width=0,height=0,stride=0,rotations=0;
  ~Impl() {
    if(queue) api.clFinish(queue);
    for(auto m:{material,occupancy,masks,bits,candidates,output}) if(m) api.clReleaseMemObject(m);
    if(kernel) api.clReleaseKernel(kernel);
    if(program) api.clReleaseProgram(program);
    if(queue) api.clReleaseCommandQueue(queue);
    if(context) api.clReleaseContext(context);
  }
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
  }
  void replace(cl_mem& memory,size_t bytes,cl_mem_flags flags,const void* data=nullptr) {
    if(!bytes || bytes>maxAllocation || bytes>512ULL*1024*1024)
      throw std::runtime_error("OpenCL buffer exceeds the GPU allocation limit (maximum 512 MiB per buffer)");
    if(memory) { api.clReleaseMemObject(memory); memory=nullptr; }
    cl_int e=0; memory=api.clCreateBuffer(context,flags|(data?CL_MEM_COPY_HOST_PTR:0),bytes,const_cast<void*>(data),&e);
    check(e,"allocate buffer");
  }
  template<class T> void arg(cl_uint index,const T& value) { check(api.clSetKernelArg(kernel,index,sizeof(value),&value),"kernel argument"); }
  std::vector<uint8_t> run(uint32_t count,uint32_t mode,uint64_t first,uint32_t rows,uint32_t step,uint32_t ww,uint32_t wh,
                         std::span<const GpuCandidate> input={}) {
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
    const size_t global=count;
    check(api.clEnqueueNDRangeKernel(queue,kernel,1,nullptr,&global,nullptr,0,nullptr,nullptr),"dispatch collision kernel");
    std::vector<uint8_t> flags(count);
    check(api.clEnqueueReadBuffer(queue,output,CL_TRUE,0,flags.size(),flags.data(),0,nullptr,nullptr),"read collision results");
    return flags;
  }
};
std::vector<GpuDeviceInfo> listGpuDevices() {
  Api api; std::vector<GpuDeviceInfo> out; for(const auto& d:devices(api)) out.push_back(d.info); return out;
}
GpuBitmap::GpuBitmap(int index) : impl_(std::make_unique<Impl>()) { impl_->initialize(index); }
GpuBitmap::~GpuBitmap()=default;
const GpuDeviceInfo& GpuBitmap::device() const { return impl_->info; }
void GpuBitmap::setSheet(uint32_t w,uint32_t h,std::span<const uint64_t> material) {
  const uint64_t stride=(uint64_t(w)+63)/64;
  if(!w || !h || material.size()!=stride*h || material.size()>UINT32_MAX) throw std::invalid_argument("Invalid GPU sheet dimensions");
  impl_->width=w; impl_->height=h; impl_->stride=uint32_t(stride); impl_->sheetBytes=material.size_bytes();
  impl_->occupancyReady=false;
  impl_->replace(impl_->material,material.size_bytes(),CL_MEM_READ_ONLY,material.data());
  impl_->replace(impl_->occupancy,material.size_bytes(),CL_MEM_READ_ONLY);
}
void GpuBitmap::setOccupancy(std::span<const uint64_t> occupancy) {
  if(occupancy.size_bytes()!=impl_->sheetBytes || !impl_->occupancy) throw std::invalid_argument("Invalid GPU occupancy size");
  check(impl_->api.clEnqueueWriteBuffer(impl_->queue,impl_->occupancy,CL_TRUE,0,occupancy.size_bytes(),occupancy.data(),0,nullptr,nullptr),"upload occupancy");
  impl_->occupancyReady=true;
}
void GpuBitmap::setMasks(std::span<const GpuMaskInfo> masks,std::span<const uint64_t> words) {
  if(masks.empty() || masks.size()>3600 || words.size()>UINT32_MAX) throw std::invalid_argument("Invalid GPU masks");
  for(const auto& m:masks) if(!m.width || !m.height || m.wordsPerRow!=(uint64_t(m.width)+63)/64 ||
      uint64_t(m.offset)+uint64_t(m.height)*m.wordsPerRow>words.size()) throw std::invalid_argument("Invalid GPU mask bounds");
  impl_->replace(impl_->masks,masks.size_bytes(),CL_MEM_READ_ONLY,masks.data());
  impl_->replace(impl_->bits,words.size_bytes(),CL_MEM_READ_ONLY,words.data());
  impl_->rotations=cl_uint(masks.size());
}
std::vector<uint8_t> GpuBitmap::filter(std::span<const GpuCandidate> candidates) {
  if(candidates.size()>262144) throw std::invalid_argument("GPU batch is too large");
  return impl_->run(uint32_t(candidates.size()),0,0,1,1,impl_->width,impl_->height,candidates);
}
std::vector<uint8_t> GpuBitmap::filterGrid(uint64_t first,uint32_t count,uint32_t rows,uint32_t step,uint32_t ww,uint32_t wh) {
  if(!rows || !step || !impl_->rotations || uint64_t(rows)*step>uint64_t(INT32_MAX) ||
      first>UINT64_MAX-count || (first+count)/impl_->rotations/rows>uint64_t(INT32_MAX)/step)
    throw std::invalid_argument("Invalid GPU grid range");
  return impl_->run(count,1,first,rows,step,ww,wh);
}
}
