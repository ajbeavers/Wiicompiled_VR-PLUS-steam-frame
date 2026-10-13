// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/frame_controls.h"
#include <iostream>

using namespace mkw::vr;
static int failures = 0;
static void Check(bool c, const char* m) { if (!c) { ++failures; std::cerr << "FAILED: " << m << '\n'; } }

static PADStatus Pad(const frame::Inputs& in, frame::Context ctx = {}) {
    return MapQuestInput(frame::Map(in, ctx));
}
static bool Has(const PADStatus& p, uint16_t b) { return (p.button & b) == b; }

int main() {
    const uint16_t all = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START |
                         PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT |
                         PAD_TRIGGER_L | PAD_TRIGGER_R | PAD_TRIGGER_Z;
    {
        frame::Inputs in;
        const auto p = Pad(in);
        Check(p.err == PAD_ERR_NONE && (p.button & all) == 0, "idle controllers press nothing");
    }
    { frame::Inputs in; in.right_trigger = 1; auto p = Pad(in);
      Check(Has(p, PAD_BUTTON_A) && p.analogA == 255, "right trigger accelerates (GC A)"); }
    { frame::Inputs in; in.right_trigger = 0.3f; Check(!Has(Pad(in), PAD_BUTTON_A), "light right trigger does not"); }
    { frame::Inputs in; in.a = true; Check(Has(Pad(in), PAD_BUTTON_A), "A accelerates"); }
    { frame::Inputs in; in.left_trigger = 1; auto p = Pad(in);
      Check(Has(p, PAD_BUTTON_B) && !Has(p, PAD_BUTTON_A), "left trigger reverses (GC B held)"); }
    { frame::Inputs in; in.left_trigger = 1; in.right_trigger = 1; auto p = Pad(in);
      Check(Has(p, PAD_BUTTON_B) && !Has(p, PAD_BUTTON_A), "reverse overrides throttle"); }
    { frame::Inputs in; in.b = true; Check(Has(Pad(in), PAD_BUTTON_B), "B brakes / backs (GC B)"); }
    { frame::Inputs in; in.x = true; auto p = Pad(in); Check(Has(p, PAD_TRIGGER_L) && p.triggerL == 255, "X uses item (GC L)"); }
    { frame::Inputs in; in.left_bumper = true; Check(Has(Pad(in), PAD_TRIGGER_L), "left bumper uses item"); }
    { frame::Inputs in; in.y = true; Check(Has(Pad(in), PAD_BUTTON_X), "Y looks back (GC X)"); }
    { frame::Inputs in; in.right_bumper = true; Check(Has(Pad(in), PAD_TRIGGER_R), "right bumper drifts (GC R)"); }
    { frame::Inputs in; in.left_squeeze = 1; Check(Has(Pad(in), PAD_TRIGGER_R), "left grip drifts"); }
    { frame::Inputs in; in.right_squeeze = 1; Check(Has(Pad(in), PAD_TRIGGER_R), "right grip drifts"); }
    { frame::Inputs in; in.right_squeeze = 1; in.left_squeeze = 1; frame::Context c; c.grips_hold_wheel = true;
      Check(!Has(Pad(in, c), PAD_TRIGGER_R), "grips hold the wheel in the cockpit, no drift");
      in.right_bumper = true; Check(Has(Pad(in, c), PAD_TRIGGER_R), "right bumper drifts in the cockpit too"); }
    { frame::Inputs in; in.dpad = {true, false, false, false}; Check(Has(Pad(in), PAD_BUTTON_UP), "D-pad up"); }
    { frame::Inputs in; in.dpad = {false, true, false, false}; Check(Has(Pad(in), PAD_BUTTON_DOWN), "D-pad down"); }
    { frame::Inputs in; in.dpad = {false, false, true, false}; Check(Has(Pad(in), PAD_BUTTON_LEFT), "D-pad left"); }
    { frame::Inputs in; in.dpad = {false, false, false, true}; Check(Has(Pad(in), PAD_BUTTON_RIGHT), "D-pad right"); }
    { frame::Inputs in; in.menu = true; Check(Has(Pad(in), PAD_BUTTON_START), "Menu is Start"); }
    { frame::Inputs in; in.left_stick_x = 1; Check(Pad(in).stickX == 100, "left stick steers"); }
    { frame::Inputs in; in.right_stick_y = 1; Check(Has(Pad(in), PAD_BUTTON_UP), "right stick up is a trick (D-pad up)"); }
    { frame::Inputs in; in.view = true; Check((Pad(in).button & all) == 0, "View held alone presses nothing yet"); }
    { frame::Context c; c.view_back_pulse = true; Check(Has(Pad({}, c), PAD_BUTTON_B), "View tap pulses B"); }
    {   // View: tap, hold, close
        frame::ViewButton v; const int64_t ms = 1'000'000;
        auto r = v.Update(true, 0, false);
        Check(!r.open_panel && !r.back_pulse, "view press does nothing at once");
        r = v.Update(true, 400 * ms, false); Check(!r.open_panel, "not yet at 0.4 s");
        r = v.Update(true, 500 * ms, false); Check(r.open_panel, "opens settings at 0.5 s");
        r = v.Update(true, 900 * ms, false); Check(!r.open_panel, "opens once");
        r = v.Update(false, 950 * ms, true); Check(!r.back_pulse, "release after a hold does not go back");
        r = v.Update(true, 2000 * ms, true); Check(r.close_press && !r.open_panel, "press while open closes");
        r = v.Update(true, 3000 * ms, true); Check(!r.open_panel, "holding after close does not reopen");
        r = v.Update(false, 3100 * ms, false); Check(!r.back_pulse, "release after close does not go back");
        v.Update(true, 4000 * ms, false);
        r = v.Update(false, 4100 * ms, false); Check(r.back_pulse, "tap goes back on release");
        r = v.Update(false, 4100 * ms + 200 * ms, false); Check(!r.back_pulse, "back is a short pulse");
    }
    std::cerr << (failures ? "frame_controls_tests FAILED\n" : "frame_controls_tests ok\n");
    return failures ? 1 : 0;
}
