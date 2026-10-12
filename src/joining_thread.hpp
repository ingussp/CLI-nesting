#pragma once

#include <thread>
#include <utility>

namespace clinesting {
// Workers use the search's shared cancellation flags. Only automatic joining
// is needed here; older Apple libc++ versions do not provide std::jthread.
class JoiningThread {
 public:
  template<class F> explicit JoiningThread(F&& work)
      : thread_(std::forward<F>(work)) {}
  JoiningThread(JoiningThread&&) noexcept = default;
  JoiningThread(const JoiningThread&) = delete;
  JoiningThread& operator=(const JoiningThread&) = delete;
  ~JoiningThread() { if(thread_.joinable()) thread_.join(); }
 private:
  std::thread thread_;
};
}
