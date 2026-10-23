#include "gesture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wintab {

GestureParams GestureParams::from_config(const Config& cfg) {
    GestureParams params;
    params.step_fraction = cfg.step_fraction;
    params.dead_zone_fraction = cfg.dead_zone_fraction;
    params.direction_dominance = cfg.direction_dominance;
    params.commit_fraction = cfg.commit_fraction;
    params.arm_window = cfg.arm_window_ms / 1000.0;
    params.tap_max_ms = cfg.tap_max_ms;
    params.tap_max_move = cfg.tap_max_move;
    params.invert = cfg.invert_direction;
    return params;
}

ThreeFingerSwipe::ThreeFingerSwipe(GestureParams params) : params_(params) {}

// --- events ---------------------------------------------------------------

std::vector<GestureEvent> ThreeFingerSwipe::feed(const ContactFrame& frame) {
    std::vector<GestureEvent> events;
    feed(frame, events);
    return events;
}

void ThreeFingerSwipe::feed(const ContactFrame& frame, std::vector<GestureEvent>& out) {
    last_frame_ = frame.time;

    if (frame.dropped) {
        if (state_ == GestureState::Tracking) out.push_back(SwipeCancel{"syn_dropped", frame.time});
        reset();
        return;
    }

    if (frame.count == 0) {
        if (state_ == GestureState::Tracking) {
            // All three fingers lifted inside one report: still a release.
            finish(frame.time, out);
        } else if (tap_ok_ && is_tap(frame.time)) {
            out.push_back(SwipeTap{frame.time});
        }
        reset();
        return;
    }

    // Four or more fingers: this touch sequence is not a three-finger swipe.
    if (frame.count > 3) {
        if (state_ == GestureState::Tracking) {
            out.push_back(SwipeCancel{"extra_finger", frame.time});
            state_ = GestureState::Blocked;
            samples_.clear();
        } else if (state_ != GestureState::Blocked) {
            state_ = GestureState::Blocked;
        }
        tap_ok_ = false;
        return;
    }

    if (state_ == GestureState::Blocked) return;

    if (state_ == GestureState::Idle) {
        seq_t0_ = frame.time;
        state_ = GestureState::Landing;
    }

    switch (state_) {
        case GestureState::Landing: landing(frame, out); break;
        case GestureState::Deciding: deciding(frame, out); break;
        case GestureState::Tracking: tracking(frame, out); break;
        default: break;  // Done: wait for the fingers to lift
    }
}

std::vector<GestureEvent> ThreeFingerSwipe::flush(double time) {
    std::vector<GestureEvent> events;
    flush(time, events);
    return events;
}

void ThreeFingerSwipe::flush(double time, std::vector<GestureEvent>& out) {
    if (state_ == GestureState::Tracking) {
        finish(time, out);
        state_ = GestureState::Done;
        return;
    }
    reset();
}

// --- states ---------------------------------------------------------------

void ThreeFingerSwipe::landing(const ContactFrame& frame, std::vector<GestureEvent>&) {
    const double elapsed = frame.time - seq_t0_;
    if (frame.count == 3 && elapsed <= params_.arm_window) {
        anchor_ = frame.centroid().value_or(std::make_pair(0.0, 0.0));
        max_move_ = 0.0;
        tap_ok_ = true;
        state_ = GestureState::Deciding;
    } else if (elapsed > params_.arm_window) {
        // The fingers did not land together: a scroll with a stray finger.
        tap_ok_ = false;
        state_ = GestureState::Blocked;
    }
}

void ThreeFingerSwipe::deciding(const ContactFrame& frame, std::vector<GestureEvent>& out) {
    if (frame.count != 3) {
        // A finger lifted before we moved anywhere: stay put.
        return;
    }

    const auto [dx, dy] = delta(frame);
    const double distance = std::hypot(dx, dy);
    max_move_ = std::max(max_move_, distance);
    if (distance < params_.dead_zone_fraction) return;

    if (std::abs(dx) < params_.direction_dominance * std::abs(dy)) {
        // Vertical or ambiguous movement: not this gesture.
        tap_ok_ = false;
        state_ = GestureState::Blocked;
        return;
    }

    tap_ok_ = false;
    state_ = GestureState::Tracking;
    progress = progress_for(dx);
    samples_.clear();
    samples_.emplace_back(frame.time, progress);
    out.push_back(SwipeBegin{frame.time});
    out.push_back(SwipeUpdate{progress, frame.time, preview_steps()});
}

void ThreeFingerSwipe::tracking(const ContactFrame& frame, std::vector<GestureEvent>& out) {
    if (frame.count != 3) {
        finish(frame.time, out);
        state_ = GestureState::Done;
        return;
    }

    const auto [dx, ignored_dy] = delta(frame);
    max_move_ = std::max(max_move_, std::abs(dx));
    progress = progress_for(dx);
    samples_.emplace_back(frame.time, progress);
    if (samples_.size() > 64) samples_.erase(samples_.begin(), samples_.begin() + 32);
    out.push_back(SwipeUpdate{progress, frame.time, preview_steps()});
}

// --- helpers --------------------------------------------------------------

int ThreeFingerSwipe::preview_steps() const { return commit_steps(progress); }

void ThreeFingerSwipe::finish(double time, std::vector<GestureEvent>& out) {
    // Velocity is reported for the log only. It deliberately does not decide
    // anything: a swipe that commits a window the outline never showed is the
    // one thing a strip of windows must never do.
    out.push_back(SwipeEnd{progress, velocity(), preview_steps(), time});
}

std::pair<double, double> ThreeFingerSwipe::delta(const ContactFrame& frame) const {
    const auto centre = frame.centroid().value_or(anchor_);
    return {centre.first - anchor_.first, centre.second - anchor_.second};
}

double ThreeFingerSwipe::progress_for(double dx) const {
    // Moving right (dx > 0) steps forward through the list.
    const double sign = params_.invert ? -1.0 : 1.0;
    return sign * dx / std::max(1e-6, params_.step_fraction);
}

double ThreeFingerSwipe::velocity() const {
    if (samples_.size() < 2) return 0.0;
    const auto last = samples_.back();
    auto first = samples_.front();
    for (auto it = samples_.rbegin() + 1; it != samples_.rend(); ++it) {
        if (last.first - it->first >= params_.velocity_window) {
            first = *it;
            break;
        }
    }
    const double dt = last.first - first.first;
    if (dt <= 1e-6) return 0.0;
    return (last.second - first.second) / dt;
}

int ThreeFingerSwipe::commit_steps(double value) const {
    return steps_at(value, params_.commit_fraction);
}

bool ThreeFingerSwipe::is_tap(double time) const {
    const double duration = (time - seq_t0_) * 1000.0;
    return duration <= params_.tap_max_ms && max_move_ <= params_.tap_max_move;
}

void ThreeFingerSwipe::reset() {
    state_ = GestureState::Idle;
    progress = 0.0;
    max_move_ = 0.0;
    tap_ok_ = false;
    samples_.clear();
}

std::string ThreeFingerSwipe::describe() const {
    static const char* names[] = {"idle", "landing", "deciding", "tracking", "done", "blocked"};
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "state=%s progress=%+.3f",
                  names[static_cast<int>(state_)], progress);
    return buffer;
}

// --- display helpers ------------------------------------------------------

int steps_at(double progress, double commit_fraction) {
    if (progress >= 0.0) {
        return static_cast<int>(std::floor(progress + 1.0 - commit_fraction + kBoundaryEpsilon));
    }
    return static_cast<int>(std::ceil(progress - 1.0 + commit_fraction - kBoundaryEpsilon));
}

int highlight_index(double progress, int index, int count, double commit_fraction) {
    if (count <= 0) return 0;
    return std::max(0, std::min(count - 1, index + steps_at(progress, commit_fraction)));
}

double apply_rubber_band(double progress, int index, int count, double resistance) {
    if (count <= 1) return progress * resistance;
    const double low = -static_cast<double>(index);
    const double high = static_cast<double>(count - 1 - index);
    if (progress > high) return high + (progress - high) * resistance;
    if (progress < low) return low + (progress - low) * resistance;
    return progress;
}

int clamp_steps(int steps, int index, int count) {
    if (count <= 0) return 0;
    return std::max(-index, std::min(count - 1 - index, steps));
}

}  // namespace wintab
