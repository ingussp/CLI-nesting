#pragma once
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace clinesting::detail {
// A bounded latest-value mailbox. Expensive reporting never holds a search lock.
// One consumer preserves publication order; a newer pending snapshot replaces
// older pending work, but never interrupts a publication already in progress.
template<class T> class LatestResultPublisher {
 public:
  explicit LatestResultPublisher(std::function<void(T)> consume)
      : consume_(std::move(consume)),worker_([this] {run();}) {}
  LatestResultPublisher(const LatestResultPublisher&)=delete;
  ~LatestResultPublisher() {try {close();} catch(...) {}}
  void submit(T value) {
    std::lock_guard lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    if(closing_) return;
    pending_=std::move(value);ready_.notify_one();
  }
  bool failed() const {return failed_.load();}
  // Drain the last pending value, join, and propagate reporting failures.
  void close() {
    {std::lock_guard lock(mutex_);closing_=true;ready_.notify_one();}
    if(worker_.joinable()) worker_.join();
    if(error_) std::rethrow_exception(error_);
  }
 private:
  void run() {
    try {
      for(;;) {
        std::optional<T> value;
        {
          std::unique_lock lock(mutex_);
          ready_.wait(lock,[&] {return closing_ || pending_.has_value();});
          if(!pending_) return;
          value=std::move(pending_);pending_.reset();
        }
        consume_(std::move(*value));
      }
    } catch(...) {
      std::lock_guard lock(mutex_);error_=std::current_exception();
      failed_=true;closing_=true;pending_.reset();
    }
  }
  std::function<void(T)> consume_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<T> pending_;
  bool closing_{false};
  std::atomic<bool> failed_{false};
  std::exception_ptr error_;
  std::thread worker_;
};
}
