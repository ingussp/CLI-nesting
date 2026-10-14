#pragma once
#include "clinesting/json_io.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <syncstream>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace clinesting::cli {
// Root JSON has its own writer so a GUI reader cannot stall search or history.
// A single pending snapshot replaces older waiting work, bounding memory use.
class LatestJsonPublisher {
 public:
  LatestJsonPublisher(std::filesystem::path output,BackgroundRequest request)
      :output_(std::filesystem::absolute(output)),temporary_(output_),request_(std::move(request)) {
    temporary_+=".tmp";
    worker_=std::thread([this] {run();});
  }
  LatestJsonPublisher(const LatestJsonPublisher&)=delete;
  LatestJsonPublisher& operator=(const LatestJsonPublisher&)=delete;
  // Exception unwinding must not wait indefinitely for an unrelated reader.
  ~LatestJsonPublisher() {
    {std::lock_guard lock(mutex_);abort_=true;ready_.notify_all();}
    if(worker_.joinable()) worker_.join();
  }
  void submit(const BackgroundRequest& request,const OrchestratorRunStats& result) {
    Snapshot snapshot{request.config.searchIteration,result};
    std::lock_guard lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    if(closing_) throw std::logic_error("Latest JSON publisher is already closed");
    pending_=std::move(snapshot);
    ready_.notify_one();
  }
  bool failed() const {return failed_.load();}
  // The final snapshot must be published before a successful process exit.
  // A busy file has no time limit; the console explains why shutdown is waiting.
  void finish() {
    {std::lock_guard lock(mutex_);closing_=true;ready_.notify_all();}
    if(blocked_.load()) std::osyncstream(std::cout)
        <<"Waiting for the latest result JSON to become available: "<<output_.string()<<"\n"<<std::flush;
    if(worker_.joinable()) worker_.join();
    if(error_) std::rethrow_exception(error_);
  }
 private:
  struct Snapshot {uint64_t iteration;OrchestratorRunStats result;};
  bool replace() {
#ifdef _WIN32
    if(MoveFileExW(temporary_.c_str(),output_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return true;
    const auto error=GetLastError();
    const auto attributes=GetFileAttributesW(output_.c_str());
    const bool permanentTarget=attributes!=INVALID_FILE_ATTRIBUTES &&
        (attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_READONLY));
    if(!permanentTarget && (error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION || error==ERROR_ACCESS_DENIED)) {
      if(!blocked_.exchange(true)) std::osyncstream(std::cout)
          <<"Latest result JSON is busy (Windows error "<<error<<"): "<<output_.string()
          <<". Waiting in background; nesting and numbered results continue.\n"<<std::flush;
      return false;
    }
    throw std::runtime_error("Cannot publish latest result JSON: "+output_.string()+
                             " (Windows error "+std::to_string(error)+")");
#else
    std::filesystem::rename(temporary_,output_);
    return true;
#endif
  }
  void run() {
    bool temporaryWritten=false;
    try {
      for(;;) {
        std::optional<Snapshot> snapshot;
        {
          std::unique_lock lock(mutex_);
          ready_.wait(lock,[&] {return abort_ || closing_ || pending_.has_value();});
          if(abort_ || !pending_) break;
          snapshot=std::move(pending_);pending_.reset();
        }
        request_.config.searchIteration=snapshot->iteration;
        std::filesystem::create_directories(output_.parent_path());
        temporaryWritten=true;
        writeNestingJson(temporary_,request_,snapshot->result);
        for(;;) {
          {
            std::lock_guard lock(mutex_);
            if(abort_) break;
            // Prefer the newest snapshot before attempting to replace a busy file.
            if(pending_) break;
          }
          if(replace()) {
            temporaryWritten=false;
            if(blocked_.exchange(false)) std::osyncstream(std::cout)
                <<"Latest result JSON is available; the newest prepared snapshot was published.\n"<<std::flush;
            break;
          }
          std::unique_lock lock(mutex_);
          ready_.wait_for(lock,std::chrono::milliseconds(50),[&] {return abort_ || pending_.has_value();});
        }
      }
    } catch(...) {
      std::lock_guard lock(mutex_);
      error_=std::current_exception();failed_=true;pending_.reset();
    }
    if(temporaryWritten) {
      std::error_code ignored;std::filesystem::remove(temporary_,ignored);
    }
  }
  std::filesystem::path output_,temporary_;
  BackgroundRequest request_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<Snapshot> pending_;
  bool closing_{false},abort_{false};
  std::atomic<bool> failed_{false},blocked_{false};
  std::exception_ptr error_;
  std::thread worker_;
};
}
