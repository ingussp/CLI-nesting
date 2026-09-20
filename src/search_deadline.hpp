#pragma once
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <functional>

namespace clinesting {
// Signal cooperative interruption of a search phase.
struct SearchTimeExpired {};

// Cooperative cancellation. Clipper/OpenCL calls finish before cancellation;
// their state and already validated placements are never interrupted halfway.
class SearchDeadline {
 public:
  // Initialize a shared deadline and cancellation hook.
  explicit SearchDeadline(double seconds, std::function<bool()> stop={}) : enabled_(seconds>0),stop_(std::move(stop)) {
    if(!std::isfinite(seconds) || seconds<0 || seconds>86400)
      throw std::invalid_argument("timeLimitSeconds must be between 0 and 86400");
    end_=std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
  }
  // Check whether the caller requested a cooperative stop.
  bool cancelled() const { return stop_ && stop_(); }
  // Check whether the configured time budget has elapsed.
  bool timedOut() const { return enabled_ && std::chrono::steady_clock::now()>=end_; }
  // Check either user cancellation or the elapsed time limit.
  bool expired() const { return cancelled() || timedOut(); }
  // Throw when a runtime operation reports an error or deadline expiry.
  void check() const { if(expired()) throw SearchTimeExpired{}; }
 private:
  bool enabled_;
  std::function<bool()> stop_;
  std::chrono::steady_clock::time_point end_;
};
}
