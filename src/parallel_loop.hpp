#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace clinesting {

// One caller and persistent helpers. A run is a barrier: captured input and
// per-worker output remain owned by the caller until every helper has finished.
class ParallelLoop {
 public:
  // Initialize persistent workers for repeated parallel loops.
  explicit ParallelLoop(size_t count) : count_(count) {
    try {
      for(size_t slot=1;slot<count_;++slot) workers_.emplace_back([this,slot] {
        size_t seen=0;
        std::unique_lock lock(mutex_);
        for(;;) {
          ready_.wait(lock,[&] { return stopping_ || epoch_!=seen; });
          if(stopping_) return;
          seen=epoch_;
          lock.unlock();
          execute(slot);
          lock.lock();
          ++finished_;
          done_.notify_one();
        }
      });
    } catch(...) { stop(); throw; }
  }
  // Stop and join every persistent worker.
  ~ParallelLoop() { stop(); }
  // Return the number of execution slots in the worker pool.
  size_t size() const { return count_; }
  // Distribute a range across the persistent worker slots and wait for completion.
  template<class F> void run(size_t count,F&& f) {
    {
      std::lock_guard lock(mutex_);
      task_=[&,count](size_t slot) { f(count*slot/count_,count*(slot+1)/count_,slot); };
      error_=nullptr;
      finished_=0;
      ++epoch_;
    }
    ready_.notify_all();
    execute(0);
    std::unique_lock lock(mutex_);
    done_.wait(lock,[&] { return finished_==workers_.size(); });
    task_={};
    if(error_) std::rethrow_exception(error_);
  }
 private:
  // Process one worker slot's assigned range and capture failures.
  void execute(size_t slot) {
    try { task_(slot); }
    catch(...) { std::lock_guard lock(mutex_); if(!error_) error_=std::current_exception(); }
  }
  // Signal shutdown and join the persistent worker threads.
  void stop() {
    { std::lock_guard lock(mutex_); stopping_=true; }
    ready_.notify_all();
    workers_.clear();
  }
  size_t count_,epoch_{0},finished_{0};
  bool stopping_{false};
  std::mutex mutex_;
  std::condition_variable ready_,done_;
  std::function<void(size_t)> task_;
  std::exception_ptr error_;
  std::vector<std::jthread> workers_;
};
}
