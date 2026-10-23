// The pure gesture state machine, one test per test in tests/test_gesture.py.
// Names are kept identical (prefixed with gesture_) so a coverage conversation
// maps 1:1 onto the Python suite.
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.hpp"
#include "gesture.hpp"
#include "support/harness.hpp"

using namespace wintab;

namespace {

//: An arbitrary monotonic origin, same as the Python suite.
constexpr double kT0 = 1000.0;

ContactFrame frame_at(double t, const std::vector<std::pair<double, double>>& points,
                      bool dropped = false) {
    ContactFrame frame;
    frame.time = kT0 + t;
    for (std::size_t i = 0; i < points.size() && frame.count < kMaxContacts; ++i) {
        frame.contacts[static_cast<std::size_t>(frame.count++)] =
            Contact{static_cast<int>(i), points[i].first, points[i].second};
    }
    frame.dropped = dropped;
    return frame;
}

ContactFrame stacked(double t, double x, double y = 0.5, int count = 3, bool dropped = false) {
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < count; ++i) points.emplace_back(x, y);
    return frame_at(t, points, dropped);
}

std::vector<GestureEvent> land(ThreeFingerSwipe& engine, double t = 0.0, double x = 0.5,
                               double y = 0.5, int fingers = 3) {
    std::vector<GestureEvent> events;
    for (int i = 1; i <= fingers; ++i) {
        for (auto& event : engine.feed(stacked(t + 0.005 * (i - 1), x, y, i))) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

//: Move the three fingers at a chosen speed (unhurried by default). The commit
//: rule must not depend on speed, so only the tests that care say otherwise.
std::pair<std::vector<GestureEvent>, double> drag_slowly(ThreeFingerSwipe& engine, double t,
                                                         double from_x, double to_x,
                                                         double duration = 0.5, double y = 0.5,
                                                         double rate = 50.0) {
    std::vector<GestureEvent> events;
    const int steps = std::max(2, static_cast<int>(duration * rate));
    for (int i = 1; i <= steps; ++i) {
        const double x = from_x + (to_x - from_x) * i / steps;
        for (auto& event : engine.feed(stacked(t + i / rate, x, y))) {
            events.push_back(std::move(event));
        }
    }
    return {std::move(events), t + duration};
}

//: Stay perfectly still, so release velocity reads as zero.
std::pair<std::vector<GestureEvent>, double> hold(ThreeFingerSwipe& engine, double t, double x,
                                                  double y = 0.5, double duration = 0.15) {
    std::vector<GestureEvent> events;
    const int steps = std::max(1, static_cast<int>(duration * 50));
    for (int i = 1; i <= steps; ++i) {
        for (auto& event : engine.feed(stacked(t + i / 50.0, x, y))) {
            events.push_back(std::move(event));
        }
    }
    return {std::move(events), t + duration};
}

std::vector<GestureEvent> release(ThreeFingerSwipe& engine, double t, double x, double y = 0.5) {
    std::vector<GestureEvent> events;
    for (auto& event : engine.feed(stacked(t, x, y, 2))) events.push_back(std::move(event));
    for (auto& event : engine.feed(stacked(t + 0.005, x, y, 1))) events.push_back(std::move(event));
    for (auto& event : engine.feed(stacked(t + 0.010, x, y, 0))) events.push_back(std::move(event));
    return events;
}

void append(std::vector<GestureEvent>& into, std::vector<GestureEvent> more) {
    for (auto& event : more) into.push_back(std::move(event));
}

std::string kind(const GestureEvent& event) {
    if (std::holds_alternative<SwipeBegin>(event)) return "Begin";
    if (std::holds_alternative<SwipeUpdate>(event)) return "Update";
    if (std::holds_alternative<SwipeEnd>(event)) return "End";
    if (std::holds_alternative<SwipeCancel>(event)) return "Cancel";
    return "Tap";
}

std::string kinds(const std::vector<GestureEvent>& events) {
    std::string text;
    for (const auto& event : events) {
        if (!text.empty()) text += ' ';
        text += kind(event);
    }
    return text;
}

std::vector<SwipeUpdate> updates(const std::vector<GestureEvent>& events) {
    std::vector<SwipeUpdate> out;
    for (const auto& event : events) {
        if (const auto* update = std::get_if<SwipeUpdate>(&event)) out.push_back(*update);
    }
    return out;
}

std::vector<SwipeEnd> ends(const std::vector<GestureEvent>& events) {
    std::vector<SwipeEnd> out;
    for (const auto& event : events) {
        if (const auto* end = std::get_if<SwipeEnd>(&event)) out.push_back(*end);
    }
    return out;
}

bool has_update(const std::vector<GestureEvent>& events) { return !updates(events).empty(); }

//: The shipped feel, read from the config rather than written out again, so
//: changing the default cannot leave these tests asserting the old one.
double step_fraction() { return GestureParams::from_config(Config{}).step_fraction; }
double commit_fraction() { return GestureParams::from_config(Config{}).commit_fraction; }

}  // namespace

// --- arm ------------------------------------------------------------------

TEST_CASE(gesture_fingers_landing_together_begin_the_swipe) {
    ThreeFingerSwipe engine;
    CHECK(land(engine).empty());
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.44);
    (void)t;
    CHECK_EQ(kinds(std::vector<GestureEvent>{events.front()}), std::string("Begin"));
    CHECK(has_update(events));
}

TEST_CASE(gesture_three_finger_tap_never_opens_the_switcher) {
    ThreeFingerSwipe engine;
    land(engine);
    hold(engine, 0.02, 0.5, 0.5, 0.05);
    CHECK_EQ(kinds(release(engine, 0.08, 0.5)), std::string("Tap"));
}

TEST_CASE(gesture_a_long_three_finger_hold_is_not_a_tap) {
    ThreeFingerSwipe engine;
    land(engine);
    hold(engine, 0.02, 0.5, 0.5, 0.5);  // 500ms, past tap_max_ms
    CHECK(release(engine, 0.55, 0.5).empty());
}

TEST_CASE(gesture_tiny_wobble_stays_inside_the_dead_zone) {
    ThreeFingerSwipe engine;
    land(engine);
    // 0.02 of the pad is under the 0.025 dead zone.
    auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.48, 0.2);
    CHECK(kinds(events).find("Begin") == std::string::npos);
    CHECK(kinds(release(engine, t, 0.48)).find("End") == std::string::npos);
}

TEST_CASE(gesture_late_third_finger_is_a_scroll_not_a_gesture) {
    ThreeFingerSwipe engine;
    engine.feed(stacked(0.0, 0.5, 0.5, 1));
    engine.feed(stacked(0.10, 0.5, 0.5, 2));
    engine.feed(stacked(0.30, 0.5, 0.5, 3));
    const auto [events, t] = drag_slowly(engine, 0.32, 0.5, 0.35);
    (void)t;
    CHECK(events.empty());
}

TEST_CASE(gesture_two_finger_scroll_is_left_alone) {
    ThreeFingerSwipe engine;
    std::vector<GestureEvent> events;
    for (int i = 1; i < 12; ++i) {
        append(events, engine.feed(stacked(i * 0.02, 0.5 - i * 0.02, 0.5, 2)));
    }
    CHECK(events.empty());
}

TEST_CASE(gesture_one_finger_drag_is_left_alone) {
    ThreeFingerSwipe engine;
    std::vector<GestureEvent> events;
    for (int i = 1; i < 20; ++i) {
        append(events, engine.feed(stacked(i * 0.01, 0.5 - i * 0.01, 0.5, 1)));
    }
    CHECK(events.empty());
}

TEST_CASE(gesture_vertical_swipe_is_not_ours) {
    ThreeFingerSwipe engine;
    land(engine);
    std::vector<GestureEvent> events;
    for (int i = 1; i < 10; ++i) {
        append(events, engine.feed(stacked(0.02 + i * 0.02, 0.5, 0.5 - i * 0.02, 3)));
    }
    CHECK(events.empty());
}

TEST_CASE(gesture_a_sequence_can_be_reused_after_the_fingers_lift) {
    ThreeFingerSwipe engine;
    land(engine);
    auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.44);
    (void)events;
    release(engine, t + 0.2, 0.44);
    CHECK(land(engine, t + 0.4).empty());
    const auto second = drag_slowly(engine, t + 0.42, 0.5, 0.44);
    CHECK_EQ(kinds(std::vector<GestureEvent>{second.first.front()}), std::string("Begin"));
}

// --- progress -------------------------------------------------------------

TEST_CASE(gesture_progress_is_one_step_per_step_fraction) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.5 + step_fraction(), 0.6);
    (void)t;
    const auto batch = updates(events);
    CHECK(!batch.empty());
    if (!batch.empty()) CHECK_NEAR(batch.back().progress, 1.0, 1e-3);
}

TEST_CASE(gesture_swiping_right_selects_the_next_window) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.6, 0.6);
    (void)t;
    const auto batch = updates(events);
    CHECK(!batch.empty());
    if (!batch.empty()) {
        CHECK(batch.back().progress > 0.0);
        CHECK_EQ(batch.back().steps, 1);
    }
}

TEST_CASE(gesture_swiping_left_goes_back) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.4, 0.6);
    (void)t;
    const auto batch = updates(events);
    CHECK(!batch.empty());
    if (!batch.empty()) {
        CHECK(batch.back().progress < 0.0);
        CHECK_EQ(batch.back().steps, -1);
    }
}

TEST_CASE(gesture_invert_direction_flips_progress) {
    GestureParams params;
    params.invert = true;
    ThreeFingerSwipe engine(params);
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.4, 0.6);
    (void)t;
    const auto batch = updates(events);
    CHECK(!batch.empty());
    if (!batch.empty()) CHECK(batch.back().progress > 0.0);
}

// --- commit ---------------------------------------------------------------

namespace {

std::vector<SwipeEnd> end_after_dragging(double dx_pad) {
    ThreeFingerSwipe engine;
    land(engine);
    const double target = 0.5 + dx_pad;
    auto [events, t] = drag_slowly(engine, 0.02, 0.5, target, 0.5);
    append(events, hold(engine, t, target).first);
    append(events, release(engine, t + 0.2, target));
    return ends(events);
}

}  // namespace

TEST_CASE(gesture_past_the_commit_fraction_moves_one_window) {
    const auto end = end_after_dragging((commit_fraction() + 0.01) * step_fraction());
    CHECK_EQ(end.size(), std::size_t(1));
    if (!end.empty()) CHECK_EQ(end.front().steps, 1);
}

TEST_CASE(gesture_just_short_of_the_commit_fraction_snaps_back) {
    const auto end = end_after_dragging((commit_fraction() - 0.01) * step_fraction());
    CHECK_EQ(end.size(), std::size_t(1));
    if (!end.empty()) CHECK_EQ(end.front().steps, 0);
}

TEST_CASE(gesture_exactly_at_the_commit_fraction_moves_one_window) {
    const auto end = end_after_dragging(commit_fraction() * step_fraction());
    CHECK_EQ(end.size(), std::size_t(1));
    if (!end.empty()) CHECK_EQ(end.front().steps, 1);
}

TEST_CASE(gesture_backwards_past_the_commit_fraction_goes_back_one) {
    const auto end = end_after_dragging(-(commit_fraction() + 0.01) * step_fraction());
    CHECK_EQ(end.size(), std::size_t(1));
    if (!end.empty()) CHECK_EQ(end.front().steps, -1);
}

TEST_CASE(gesture_backwards_just_short_snaps_back) {
    const auto end = end_after_dragging(-(commit_fraction() - 0.01) * step_fraction());
    CHECK_EQ(end.size(), std::size_t(1));
    if (!end.empty()) CHECK_EQ(end.front().steps, 0);
}

TEST_CASE(gesture_two_full_steps_advance_two_windows) {
    const auto end = end_after_dragging(1.9 * step_fraction());
    CHECK_EQ(end.size(), std::size_t(1));
    if (!end.empty()) CHECK_EQ(end.front().steps, 2);
}

TEST_CASE(gesture_speed_never_changes_the_commit) {
    // A quick flick must not switch to a window the outline never showed: the
    // outline is driven by SwipeUpdate.steps, so if speed added a step at
    // release you would watch card 0 stay outlined and get card 1.
    for (const double duration : {0.03, 0.06, 0.12, 0.5}) {
        ThreeFingerSwipe engine;
        land(engine);
        const double target = 0.5 + 0.35 * step_fraction();  // short of the commit
        auto [events, t] = drag_slowly(engine, 0.02, 0.5, target, duration, 0.5, 120.0);
        append(events, release(engine, t, target));
        const auto batch = updates(events);
        const auto finished = ends(events);
        CHECK_EQ(finished.size(), std::size_t(1));
        CHECK(!batch.empty());
        if (!batch.empty() && !finished.empty()) {
            CHECK_EQ(batch.back().steps, 0);
            CHECK_EQ(finished.front().steps, batch.back().steps);
        }
    }
}

TEST_CASE(gesture_same_short_drag_without_speed_snaps_back) {
    // Short of the commit fraction, so it must not commit no matter how long
    // the fingers rest there.
    const double target = 0.5 + (commit_fraction() - 0.15) * step_fraction();
    ThreeFingerSwipe engine;
    land(engine);
    auto [events, t] = drag_slowly(engine, 0.02, 0.5, target, 0.6);
    append(events, hold(engine, t, target).first);
    append(events, release(engine, t + 0.2, target));
    const auto finished = ends(events);
    CHECK_EQ(finished.size(), std::size_t(1));
    if (!finished.empty()) CHECK_EQ(finished.front().steps, 0);
}

TEST_CASE(gesture_slow_drag_past_nothing_does_not_advance) {
    ThreeFingerSwipe engine;
    land(engine);
    auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.49, 0.4);
    append(events, hold(engine, t, 0.49).first);
    append(events, release(engine, t + 0.2, 0.49));
    for (const auto& end : ends(events)) CHECK_EQ(end.steps, 0);
}

TEST_CASE(gesture_all_fingers_lifting_in_one_frame_still_ends) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.44);
    (void)events;
    CHECK_EQ(kinds(engine.feed(stacked(t + 0.02, 0.5, 0.5, 0))), std::string("End"));
}

TEST_CASE(gesture_flush_ends_a_live_gesture) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.44);
    (void)events;
    CHECK(engine.active());
    CHECK_EQ(kinds(engine.flush(t + 0.02)), std::string("End"));
    CHECK(!engine.active());
}

// --- cancel ---------------------------------------------------------------

TEST_CASE(gesture_fourth_finger_cancels) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.44);
    CHECK(kinds(events).find("Begin") != std::string::npos);
    const auto cancels = engine.feed(stacked(t + 0.02, 0.44, 0.5, 4));
    CHECK_EQ(kinds(cancels), std::string("Cancel"));
    CHECK(!engine.active());
}

TEST_CASE(gesture_no_gesture_ever_starts_after_four_fingers) {
    ThreeFingerSwipe engine;
    engine.feed(stacked(0.0, 0.5, 0.5, 4));
    engine.feed(stacked(0.01, 0.5, 0.5, 3));
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.35);
    (void)t;
    CHECK(events.empty());
}

TEST_CASE(gesture_release_after_cancel_does_not_commit) {
    ThreeFingerSwipe engine;
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.44);
    (void)events;
    engine.feed(stacked(t + 0.02, 0.44, 0.5, 4));
    CHECK(release(engine, t + 0.04, 0.44).empty());
}

TEST_CASE(gesture_dropped_events_cancel_instead_of_jumping) {
    ThreeFingerSwipe engine;
    land(engine);
    drag_slowly(engine, 0.02, 0.5, 0.44);
    const auto events = engine.feed(frame_at(0.5, {}, true));
    CHECK_EQ(kinds(events), std::string("Cancel"));
    if (!events.empty()) {
        if (const auto* cancel = std::get_if<SwipeCancel>(&events.front())) {
            CHECK_EQ(cancel->reason, std::string("syn_dropped"));
        }
    }
}

// --- the card that is outlined is the card that opens ---------------------

TEST_CASE(gesture_shipped_defaults) {
    const Config config;
    const GestureParams params = GestureParams::from_config(config);
    CHECK_NEAR(params.step_fraction, config.step_fraction, 1e-12);
    CHECK_NEAR(params.commit_fraction, config.commit_fraction, 1e-12);
    CHECK(!params.invert);
    // A card is a deliberate movement, not a haul.
    CHECK(params.step_fraction < 0.2);
    // ...and the strip has to open before the outline can tip over.
    CHECK(params.dead_zone_fraction < params.commit_fraction * params.step_fraction);
}

TEST_CASE(gesture_opening_the_strip_never_selects_another_window) {
    const Config config;
    const GestureParams params = GestureParams::from_config(config);
    ThreeFingerSwipe engine(params);
    land(engine);
    // Stop just past the dead zone, the earliest the strip can appear.
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.5 + config.dead_zone_fraction * 1.01,
                                         0.3);
    (void)t;
    const auto batch = updates(events);
    CHECK(!batch.empty());
    if (!batch.empty()) {
        CHECK_EQ(batch.front().steps, 0);
        CHECK(batch.front().progress < params.commit_fraction);
    }
}

TEST_CASE(gesture_highlight_moves_with_the_fingers) {
    CHECK_EQ(highlight_index(0.6, 1, 5), 2);
    CHECK_EQ(highlight_index(-0.6, 3, 5), 2);
}

TEST_CASE(gesture_highlight_tips_over_at_half_a_step) {
    CHECK_EQ(highlight_index(0.49, 2, 6), 2);
    CHECK_EQ(highlight_index(0.5, 2, 6), 3);
    CHECK_EQ(highlight_index(-0.49, 2, 6), 2);
    CHECK_EQ(highlight_index(-0.5, 2, 6), 1);
}

TEST_CASE(gesture_highlight_clamps_at_the_ends) {
    CHECK_EQ(highlight_index(9.0, 4, 5), 4);
    CHECK_EQ(highlight_index(-9.0, 0, 5), 0);
    CHECK_EQ(highlight_index(1.0, 0, 0), 0);
}

TEST_CASE(gesture_highlight_matches_the_commit_rule) {
    ThreeFingerSwipe engine;
    const int middle = 50;  // far from either end, so clamping cannot mask a mismatch
    for (int hundredths = -250; hundredths <= 250; hundredths += 7) {
        const double progress = hundredths / 100.0;
        engine.progress = progress;
        CHECK_EQ(highlight_index(progress, middle, 99) - middle, engine.preview_steps());
    }
}

TEST_CASE(gesture_update_selection_at_a_distance) {
    const Config config;
    ThreeFingerSwipe engine(GestureParams::from_config(config));
    land(engine);
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.5 + 1.4 * config.step_fraction, 0.8);
    (void)t;
    const auto batch = updates(events);
    CHECK(!batch.empty());
    if (!batch.empty()) {
        CHECK_NEAR(batch.back().progress, 1.4, 1e-2);
        CHECK_EQ(batch.back().steps, 1);  // the second window, not the third
    }
}

TEST_CASE(gesture_end_selection_and_update_selection_agree) {
    const Config config;
    ThreeFingerSwipe engine(GestureParams::from_config(config));
    land(engine);
    const double target = 0.5 + config.step_fraction;
    auto [events, t] = drag_slowly(engine, 0.02, 0.5, target, 0.8);
    append(events, hold(engine, t, target).first);
    append(events, release(engine, t + 0.2, target));
    const auto batch = updates(events);
    const auto finished = ends(events);
    CHECK_EQ(finished.size(), std::size_t(1));
    CHECK(!batch.empty());
    if (!finished.empty() && !batch.empty()) CHECK_EQ(finished.front().steps, batch.back().steps);
}

// --- rubber band ----------------------------------------------------------

TEST_CASE(gesture_rubber_band_inside_the_range_is_untouched) {
    CHECK_NEAR(apply_rubber_band(1.5, 0, 4), 1.5, 1e-12);
}

TEST_CASE(gesture_rubber_band_past_the_end_is_compressed) {
    // index 3 of 4: nothing beyond it, so all overshoot is squeezed.
    CHECK_NEAR(apply_rubber_band(5.0, 3, 4), 1.75, 1e-12);
    // index 0 of 4: up to 3 steps are free, the rest is squeezed.
    CHECK_NEAR(apply_rubber_band(5.0, 0, 4), 3.0 + 2.0 * 0.35, 1e-12);
}

TEST_CASE(gesture_rubber_band_past_the_start_is_compressed) {
    // index 2 of 4: 2 steps back are free, the third is squeezed.
    CHECK_NEAR(apply_rubber_band(-3.0, 2, 4), -2.35, 1e-12);
}

TEST_CASE(gesture_rubber_band_single_app_cannot_move) {
    CHECK_NEAR(apply_rubber_band(3.0, 0, 1), 3.0 * 0.35, 1e-12);
}

TEST_CASE(gesture_clamp_keeps_steps_inside_the_list) {
    CHECK_EQ(clamp_steps(5, 0, 3), 2);
    CHECK_EQ(clamp_steps(-5, 2, 3), -2);
    CHECK_EQ(clamp_steps(1, 1, 3), 1);
    CHECK_EQ(clamp_steps(1, 0, 0), 0);
}

// --- replay ---------------------------------------------------------------

TEST_CASE(gesture_same_input_same_decision) {
    auto run = []() {
        ThreeFingerSwipe engine;
        land(engine);
        auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.42, 0.35);
        append(events, hold(engine, t, 0.42).first);
        append(events, release(engine, t + 0.2, 0.42));
        std::vector<int> steps;
        for (const auto& end : ends(events)) steps.push_back(end.steps);
        return steps;
    };
    CHECK_EQ(run(), run());
}

// --- beyond the Python suite ----------------------------------------------

TEST_CASE(gesture_velocity_is_reported_but_never_changes_the_step) {
    ThreeFingerSwipe engine;
    land(engine);
    // Two samples well past the dead zone, 10ms apart.
    engine.feed(stacked(0.1, 0.5 + 0.3 * engine.params().step_fraction));
    engine.feed(stacked(0.11, 0.5 + 0.63 * engine.params().step_fraction));
    const auto finished = ends(engine.feed(stacked(0.12, 0.5, 0.5, 0)));
    CHECK_EQ(finished.size(), std::size_t(1));
    if (!finished.empty()) {
        CHECK(finished.front().velocity > 1.0);
        CHECK_EQ(finished.front().steps, 1);
    }
}

TEST_CASE(gesture_a_fifth_finger_never_unblocks_the_sequence) {
    ThreeFingerSwipe engine;
    engine.feed(stacked(0.0, 0.5, 0.5, 5));
    const auto [events, t] = drag_slowly(engine, 0.02, 0.5, 0.35);
    (void)t;
    CHECK(events.empty());
    CHECK_EQ(engine.state(), GestureState::Blocked);
}
