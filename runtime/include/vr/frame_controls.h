// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Steam Frame controller layout as pure functions, so the table test can check it without OpenXR.
//
//   right trigger, A      accelerate        left trigger   reverse
//   B                     brake / back      X, left bumper use item
//   Y                     look back         both grips, right bumper   drift
//   left D-pad            D-pad             right Menu     Start
//   left View             tap: back (B), hold 0.5 s: VR settings, press while open: close

#include "vr/quest_input.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace mkw::vr::frame {

struct Inputs {
    float right_trigger = 0, left_trigger = 0;
    float right_squeeze = 0, left_squeeze = 0;
    bool a = false, b = false, x = false, y = false;
    bool menu = false, view = false;
    bool left_bumper = false, right_bumper = false;
    std::array<bool, 4> dpad{}; // up, down, left, right
    float left_stick_x = 0, left_stick_y = 0, right_stick_x = 0, right_stick_y = 0;
};

struct Context {
    bool grips_hold_wheel = false; // cockpit with hand steering on
    bool view_back_pulse = false;  // a View tap, sent as B for a short pulse after release
};

// Fills the gameplay fields of a QuestInput (everything but poses and wheel state).
inline QuestInput Map(const Inputs& in, const Context& ctx, QuestInput base = {}) noexcept {
    QuestInput q = base;
    q.active = true;
    q.cockpit_controls = false;
    q.steering_x = in.left_stick_x;
    q.steering_y = in.left_stick_y;
    q.tricks_x = in.right_stick_x;
    q.tricks_y = in.right_stick_y;
    q.accelerate = std::max(in.right_trigger, in.a ? 1.0f : 0.0f);
    q.confirm = false;
    q.reverse = in.left_trigger > 0.5f;
    q.brake = in.b || ctx.view_back_pulse;
    q.item = (in.x || in.left_bumper) ? 1.0f : 0.0f;
    q.look_back = in.y;
    q.drift = (in.right_bumper || (!ctx.grips_hold_wheel && std::max(in.left_squeeze, in.right_squeeze) > 0.5f)) ? 1.0f : 0.0f;
    q.trick = false;
    q.tricks_y = std::clamp(q.tricks_y + (in.dpad[0] ? 1.f : 0.f) - (in.dpad[1] ? 1.f : 0.f), -1.f, 1.f);
    q.tricks_x = std::clamp(q.tricks_x + (in.dpad[3] ? 1.f : 0.f) - (in.dpad[2] ? 1.f : 0.f), -1.f, 1.f);
    q.pause = in.menu;
    return q;
}

// View button: a tap goes back on release, a hold of kHoldNs opens the VR settings, a press while
// the settings are open closes them (and never also goes back).
class ViewButton {
public:
    static constexpr int64_t kHoldNs = 500'000'000, kPulseNs = 120'000'000;
    struct Result {
        bool open_panel = false;  // hold completed: open the settings
        bool close_press = false; // pressed while open: close them
        bool back_pulse = false;  // tap released recently: B
    };
    Result Update(bool down, int64_t now, bool panel_open) noexcept {
        Result r;
        const bool pressed = down && !held_;
        if (pressed) {
            down_at_ = now;
            consumed_ = panel_open;
            r.close_press = panel_open;
        }
        if (down && !consumed_ && now - down_at_ >= kHoldNs) {
            r.open_panel = true;
            consumed_ = true;
        }
        if (!down && held_ && !consumed_) pulse_until_ = now + kPulseNs;
        held_ = down;
        r.back_pulse = now < pulse_until_;
        return r;
    }
    void Reset() noexcept { held_ = consumed_ = false; down_at_ = pulse_until_ = 0; }
private:
    bool held_ = false, consumed_ = false;
    int64_t down_at_ = 0, pulse_until_ = 0;
};

} // namespace mkw::vr::frame
