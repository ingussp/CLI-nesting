#pragma once
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <functional>

namespace deepnest {
struct SearchTimeExpired {};

// Cooperative cancellation. Clipper/OpenCL calls finish before cancellation;
// their state and already validated placements are never interrupted halfway.
class SearchDeadline {
 public:
  explicit SearchDeadline(double seconds, std::function<bool()> stop={}) : enabled_(seconds>0),stop_(std::move(stop)) {
    if(!std::isfinite(seconds) || seconds<0 || seconds>86400)
      throw std::invalid_argument("timeLimitSeconds must be between 0 and 86400");
    end_=std::chrono::steady_clock::now()+std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
  }
  bool cancelled() const { return stop_ && stop_(); }
  bool timedOut() const { return enabled_ && std::chrono::steady_clock::now()>=end_; }
  bool expired() const { return cancelled() || timedOut(); }
  void check() const { if(expired()) throw SearchTimeExpired{}; }
 private:
  bool enabled_;
  std::function<bool()> stop_;
  std::chrono::steady_clock::time_point end_;
};
}
