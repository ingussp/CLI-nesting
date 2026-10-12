#include "joining_thread.hpp"
#include "parallel_loop.hpp"
#include <atomic>
#include <stdexcept>
#include <vector>

int main() {
  std::atomic<int> completed{0};
  try {
    std::vector<clinesting::JoiningThread> workers;
    // Reallocation must transfer ownership; unwinding must wait for all work.
    for(int i=0;i<16;++i) workers.emplace_back([&] { ++completed; });
    throw std::runtime_error("unwind");
  } catch(const std::runtime_error&) {}
  if(completed!=16) return 1;

  clinesting::ParallelLoop pool(4);
  std::atomic<int> visited{0};
  pool.run(100,[&](size_t begin,size_t end,size_t) { visited+=int(end-begin); });
  if(visited!=100) return 2;
  try {
    pool.run(4,[](size_t,size_t,size_t slot) {
      if(slot==1) throw std::runtime_error("worker failure");
    });
    return 3;
  } catch(const std::runtime_error&) {}
  // A worker failure must leave the persistent pool usable and joinable.
  pool.run(100,[&](size_t begin,size_t end,size_t) { visited+=int(end-begin); });
  return visited==200 ? 0 : 4;
}
