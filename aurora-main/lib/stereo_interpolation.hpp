#pragma once

#include <algorithm>
#include <cstdint>

#include <cmath>

namespace aurora::stereo {
// The current scene belongs to the VI presentation boundary. Delaying scene
// motion by one guest interval lets every display sample fall between known
// endpoints; head tracking is applied afterwards at its predicted display time.
inline float interpolation_weight(uint64_t displayTime, uint64_t boundary, uint64_t interval) noexcept {
  if (displayTime == 0 || boundary == 0 || interval == 0)
    return 1.0f;
  if (displayTime <= boundary)
    return 0.0f;
  return static_cast<float>(std::min(static_cast<double>(displayTime - boundary) / static_cast<double>(interval), 1.0));
}

// interpolation_weight assumes the display time falls inside the current frame's interval. On a
// headset it does not: a set is located for a display time one or two refreshes ahead (more with
// pipelined pacing), so display minus boundary lay beyond one interval for almost every set and the
// weight stuck at 1: each game frame was shown on two refreshes at 120 Hz and moving scenery doubled.
// SceneClock samples the scene at the display time minus a delay it learns: over a game interval the
// sets' (display - boundary) sweep one interval, so their running mean minus half an interval puts
// every sample between the previous and the current frame. A slow mean (about 2 s at 120 Hz) keeps
// the delay steady, so the scene advances uniformly; a jump (a new display lead, a different game
// rate) restarts it. One instance per caller thread.
class SceneClock {
public:
  float weight(uint64_t displayTime, uint64_t boundary, uint64_t interval) noexcept {
    if (displayTime == 0 || boundary == 0 || interval == 0)
      return 1.0f;
    const double x = static_cast<double>(displayTime) - static_cast<double>(boundary);
    const double period = static_cast<double>(interval);
    if (!m_valid || std::abs(x - m_mean) > 2.0 * period || std::abs(period - m_interval) > 0.25 * m_interval) {
      m_valid = true;
      m_mean = x;
      m_interval = period;
    } else {
      m_mean += (x - m_mean) * kRate;
    }
    return static_cast<float>(std::clamp(0.5 + (x - m_mean) / period, 0.0, 1.0));
  }
  void reset() noexcept { m_valid = false; }

private:
  static constexpr double kRate = 1.0 / 256.0;
  bool m_valid = false;
  double m_mean = 0.0;
  double m_interval = 0.0;
};
} // namespace aurora::stereo
