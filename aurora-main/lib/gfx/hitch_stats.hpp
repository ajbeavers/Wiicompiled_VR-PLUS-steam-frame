#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Game-frame hitch accounting (lane E, KartFrame). Cheap always-on counters: a few relaxed atomic
// adds where the work happens; end_frame() (aurora.cpp) logs one "hitch:" line for every game frame
// whose interval since the previous end_frame exceeds 20 ms, with the deltas of these counters over
// that interval. frame/perf/hitches.py reads the lines. Counters are shared by the game thread, the
// frame worker and the pipeline workers, so a delta says what happened in the process during the
// slow frame, not necessarily on the game thread.
namespace aurora::gfx::hitch {
inline std::atomic<uint64_t> g_pipelineWaitNs{0};  // render/worker threads blocked on a queued pipeline
inline std::atomic<uint64_t> g_pipelineSyncNs{0};  // pipelines compiled inline (not on a pipeline worker)
inline std::atomic<uint64_t> g_pipelineSyncCount{0};
inline std::atomic<uint64_t> g_texConvertNs{0};    // CPU texture conversion (GX formats to RGBA8 etc.)
inline std::atomic<uint64_t> g_texConvertBytes{0}; // bytes of GX texture data converted
inline std::atomic<uint64_t> g_texUploadBytes{0};  // bytes staged for texture uploads

inline uint64_t now_ns() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Adds the scope's duration to a counter.
class ScopedNs {
public:
  explicit ScopedNs(std::atomic<uint64_t>& counter) noexcept : counter_(counter), start_(now_ns()) {}
  ~ScopedNs() { counter_.fetch_add(now_ns() - start_, std::memory_order_relaxed); }
  ScopedNs(const ScopedNs&) = delete;
  ScopedNs& operator=(const ScopedNs&) = delete;

private:
  std::atomic<uint64_t>& counter_;
  uint64_t start_;
};
} // namespace aurora::gfx::hitch
