// The three-finger horizontal swipe state machine.
//
// Deliberately pure: it takes ContactFrames and returns gesture events. No X11,
// no GTK, no threads, so the rules (fingers land together, never four,
// horizontal only, dead zone, commit at half a card) are covered with synthetic
// frames.
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "config.hpp"
#include "touchpad.hpp"

namespace wintab {

struct GestureParams {
    double step_fraction = 0.095;
    double dead_zone_fraction = 0.025;
    double direction_dominance = 1.15;
    double commit_fraction = 0.5;
    double arm_window = 0.040;
    double tap_max_ms = 300.0;
    double tap_max_move = 0.015;
    bool invert = false;
    double velocity_window = 0.080;

    static GestureParams from_config(const Config& cfg);
};

// --- events ---------------------------------------------------------------

struct SwipeBegin {
    double time = 0.0;
};

struct SwipeUpdate {
    //: Progress in window steps; +1.0 means "one card to the right".
    double progress = 0.0;
    double time = 0.0;
    int steps = 0;
};

struct SwipeEnd {
    double progress = 0.0;
    double velocity = 0.0;
    int steps = 0;
    double time = 0.0;
};

struct SwipeCancel {
    std::string reason;
    double time = 0.0;
};

struct SwipeTap {
    double time = 0.0;
};

using GestureEvent = std::variant<SwipeBegin, SwipeUpdate, SwipeEnd, SwipeCancel, SwipeTap>;

// --- state machine --------------------------------------------------------

enum class GestureState { Idle, Landing, Deciding, Tracking, Done, Blocked };

class ThreeFingerSwipe {
public:
    explicit ThreeFingerSwipe(GestureParams params = {});

    void feed(const ContactFrame& frame, std::vector<GestureEvent>& out);
    std::vector<GestureEvent> feed(const ContactFrame& frame);

    //: Called when input stops (device loss, shutdown): ends a live gesture
    //: safely instead of leaving the overlay on screen.
    void flush(double time, std::vector<GestureEvent>& out);
    std::vector<GestureEvent> flush(double time);

    bool active() const { return state_ == GestureState::Tracking; }
    GestureState state() const { return state_; }
    const GestureParams& params() const { return params_; }
    int preview_steps() const;
    std::string describe() const;

    //: Test seam: the outline and the commit rule must agree at any progress,
    //: and the Python suite assigns this directly.
    double progress = 0.0;

private:
    void landing(const ContactFrame& frame, std::vector<GestureEvent>& out);
    void deciding(const ContactFrame& frame, std::vector<GestureEvent>& out);
    void tracking(const ContactFrame& frame, std::vector<GestureEvent>& out);
    void finish(double time, std::vector<GestureEvent>& out);
    void reset();

    std::pair<double, double> delta(const ContactFrame& frame) const;
    double progress_for(double dx) const;
    double velocity() const;
    int commit_steps(double value) const;
    bool is_tap(double time) const;

    GestureParams params_;
    GestureState state_ = GestureState::Idle;
    double seq_t0_ = 0.0;
    std::pair<double, double> anchor_{0.0, 0.0};
    double max_move_ = 0.0;
    std::deque<std::pair<double, double>> samples_;
    double last_frame_ = 0.0;
    bool tap_ok_ = false;
};

// --- display helpers ------------------------------------------------------

//: Slack added before the floor/ceil, so a progress that lands exactly on the
//: commit fraction counts as past it.
inline constexpr double kBoundaryEpsilon = 1e-9;

//: The one commit rule: which card away this progress points at. Shared by the
//: engine and by highlight_index so the outlined card and the opened card are
//: the same card by construction.
int steps_at(double progress, double commit_fraction);

//: Which window is outlined at this progress, as an absolute index.
int highlight_index(double progress, int index, int count, double commit_fraction = 0.5);

//: Compress progress past the first/last app instead of looping.
double apply_rubber_band(double progress, int index, int count, double resistance = 0.35);

//: Keep a committed step inside the list of apps.
int clamp_steps(int steps, int index, int count);

}  // namespace wintab
