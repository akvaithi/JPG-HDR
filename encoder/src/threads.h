// Tiny fork/join helper. std::thread only, so the binary stays free of
// OpenMP/TBB runtime dependencies that would have to ship with the plugin.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <thread>
#include <vector>

namespace iso21496 {

inline unsigned defaultThreadCount() {
  unsigned n = std::thread::hardware_concurrency();
  return n == 0 ? 4u : n;
}

// Runs body(i) for i in [0, count), splitting the range across `threads`
// workers. Exceptions thrown by any worker are rethrown on the calling thread.
inline void parallelFor(size_t count, unsigned threads,
                        const std::function<void(size_t)>& body) {
  if (count == 0) return;
  if (threads == 0) threads = defaultThreadCount();
  threads = static_cast<unsigned>(
      std::min<size_t>(threads, std::max<size_t>(1, count)));
  if (threads <= 1) {
    for (size_t i = 0; i < count; ++i) body(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::vector<std::exception_ptr> errors(threads);
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      try {
        for (;;) {
          size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= count) break;
          body(i);
        }
      } catch (...) {
        errors[t] = std::current_exception();
      }
    });
  }
  for (auto& th : pool) th.join();
  for (auto& e : errors)
    if (e) std::rethrow_exception(e);
}

}  // namespace iso21496
