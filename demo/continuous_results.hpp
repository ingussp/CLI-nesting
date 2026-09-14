#pragma once
#include "deepnestcpp/json_io.hpp"
#include "deepnestcpp/dxf_export.hpp"
#include "svg_preview.hpp"
#include <filesystem>
#include <cwctype>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace deepnest::cli {
namespace fs=std::filesystem;
// MSVC weakly_canonical can attempt to open a missing leaf and report access
// denied in restricted Windows environments. Canonicalize existing ancestors
// instead; genuine status/permission errors still propagate.
inline fs::path resolvePath(const fs::path& path) {
  const auto absolute=fs::absolute(path);
  if(fs::exists(absolute)) return fs::canonical(absolute);
  const auto parent=absolute.parent_path();
  if(parent==absolute || parent.empty()) throw std::runtime_error("Cannot resolve output path");
  return (resolvePath(parent)/absolute.filename()).lexically_normal();
}
inline bool isLink(const fs::path& path) {
#ifdef _WIN32
  const auto attributes=GetFileAttributesW(path.c_str());
  if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
  return fs::is_symlink(fs::symlink_status(path));
}
inline bool within(const fs::path& child,const fs::path& parent) {
  auto c=child.begin();
  for(auto p=parent.begin();p!=parent.end();++p,++c) {
    if(c==child.end()) return false;
#ifdef _WIN32
    auto a=c->wstring(),b=p->wstring();
    for(auto& ch:a) ch=std::towlower(ch);
    for(auto& ch:b) ch=std::towlower(ch);
    if(a!=b) return false;
#else
    if(*c!=*p) return false;
#endif
  }
  return true;
}
// Held outside results so a second running process cannot clear the first's history.
class DirectoryLock {
 public:
  explicit DirectoryLock(const fs::path& path) {
    if(isLink(path)) throw std::runtime_error("Results lock must not be a filesystem link");
#ifdef _WIN32
    handle_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                        FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
    if(handle_==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot lock results: another nesting process may be using this directory");
#else
    handle_=::open(path.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if(handle_<0) throw std::runtime_error("Cannot open results lock");
    if(flock(handle_,LOCK_EX|LOCK_NB)!=0) { ::close(handle_); handle_=-1; throw std::runtime_error("Results directory is already in use"); }
#endif
  }
  ~DirectoryLock() {
#ifdef _WIN32
    if(handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if(handle_>=0) { flock(handle_,LOCK_UN); ::close(handle_); }
#endif
  }
  DirectoryLock(const DirectoryLock&)=delete;
  DirectoryLock& operator=(const DirectoryLock&)=delete;
 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int handle_{-1};
#endif
};

class ResultDirectory {
 public:
  ResultDirectory(const fs::path& workingDirectory,const fs::path& input)
      :base_(fs::canonical(workingDirectory)),folder_(base_/"results"),lock_(base_/".deepnestcpp-results.lock") {
    // Preflight the complete tree before deleting anything. Refuse junctions,
    // symlinks and inputs inside the target, including Windows case aliases.
    if(within(fs::weakly_canonical(input),folder_))
      throw std::runtime_error("Continuous input must be outside the results directory, which is cleared on startup");
    if(isLink(folder_)) throw std::runtime_error("Results directory must not be a filesystem link or junction");
    if(fs::exists(folder_)) {
      if(!fs::is_directory(folder_)) throw std::runtime_error("results exists but is not a directory");
      for(const auto& entry:fs::recursive_directory_iterator(folder_)) {
        if(isLink(entry.path()) || !within(fs::weakly_canonical(entry.path()),folder_))
          throw std::runtime_error("Refusing to clear results containing a filesystem link or junction");
        if(!entry.is_regular_file() && !entry.is_directory())
          throw std::runtime_error("Unsupported file type inside results");
      }
      if(folder_.parent_path()!=base_ || folder_.filename()!="results" || fs::canonical(folder_)!=folder_)
        throw std::runtime_error("Unsafe results directory target");
      fs::remove_all(folder_);
    }
    fs::create_directory(folder_);
  }
  const fs::path& path() const { return folder_; }
  void save(size_t sequence,const BackgroundRequest& request,const OrchestratorRunStats& result,bool withDxf,bool withSvg=false) {
    const auto stem=folder_/("result"+std::to_string(sequence));
    const fs::path json=stem.string()+".json",dxf=stem.string()+".dxf";
    const fs::path jsonTemp=stem.string()+".json.tmp",dxfTemp=stem.string()+".dxf.tmp";
    const fs::path svg=stem.string()+".svg",svgTemp=stem.string()+".svg.tmp";
    if(fs::exists(json) || fs::exists(dxf) || fs::exists(svg)) throw std::runtime_error("Result sequence already exists");
    bool dxfPublished=false,svgPublished=false;
    try {
      writeNestingJson(jsonTemp,request,result);
      if(withSvg) { exportSvg(svgTemp,request.sheets,request.individual.placement,result.placement); fs::rename(svgTemp,svg); svgPublished=true; }
      if(withDxf) {
        exportPlacementResultToDxf(dxfTemp,request.sheets,request.individual.placement,result.placement);
        fs::rename(dxfTemp,dxf);
        dxfPublished=true;
      }
      // JSON is the completion marker for a fully written result pair.
      fs::rename(jsonTemp,json);
    } catch(...) {
      std::error_code ignored;
      fs::remove(jsonTemp,ignored); fs::remove(dxfTemp,ignored);
      fs::remove(svgTemp,ignored); if(svgPublished) fs::remove(svg,ignored);
      if(dxfPublished) fs::remove(dxf,ignored);
      throw;
    }
  }
 private:
  fs::path base_,folder_;
  DirectoryLock lock_;
};
}
