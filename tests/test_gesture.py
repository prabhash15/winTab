"""Unit tests for the pure gesture state machine (no hardware needed).

    python3 -m unittest discover -s tests -v
"""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from wintab.config import Config  # noqa: E402
from wintab.gesture import (  # noqa: E402
    GestureParams,
    SwipeBegin,
    SwipeCancel,
    SwipeEnd,
    SwipeTap,
    SwipeUpdate,
    ThreeFingerSwipe,
    apply_rubber_band,
    clamp_steps,
    highlight_index,
)
from wintab.touchpad import Contact, ContactFrame  # noqa: E402

T0 = 1000.0  # an arbitrary monotonic origin


def frame(t: float, points, dropped: bool = False) -> ContactFrame:
    contacts = tuple(Contact(i, float(x), float(y)) for i, (x, y) in enumerate(points))
    return ContactFrame(T0 + t, contacts, dropped=dropped)


def types(events) -> list[type]:
    return [type(e) for e in events]


def land(engine: ThreeFingerSwipe, t: float = 0.0, x: float = 0.5, y: float = 0.5, fingers: int = 3):
    """Put `fingers` fingers down one at a time, quickly enough to arm."""
    events = []
    for i in range(1, fingers + 1):
        events += engine.feed(frame(t + 0.005 * (i - 1), [(x, y)] * i))
    return events


def drag_slowly(
    engine: ThreeFingerSwipe,
    t: float,
    from_x: float,
    to_x: float,
    duration: float = 0.5,
    y: float = 0.5,
    rate: float = 50.0,
):
    """Move the three fingers at a chosen speed (unhurried by default).

    The commit rule must not depend on how fast the fingers moved, so most
    tests keep this slow and the ones that care about speed say so.
    """
    events = []
    steps = max(2, int(duration * rate))
    for i in range(1, steps + 1):
        x = from_x + (to_x - from_x) * i / steps
        events += engine.feed(frame(t + i / rate, [(x, y)] * 3))
    return events, t + duration


def hold(engine: ThreeFingerSwipe, t: float, x: float, y: float = 0.5, duration: float = 0.15):
    """Stay perfectly still, so release velocity reads as zero."""
    events = []
    steps = max(1, int(duration * 50))
    for i in range(1, steps + 1):
        events += engine.feed(frame(t + i / 50, [(x, y)] * 3))
    return events, t + duration


def release(engine: ThreeFingerSwipe, t: float, x: float, y: float = 0.5):
    events = engine.feed(frame(t, [(x, y)] * 2))
    events += engine.feed(frame(t + 0.005, [(x, y)]))
    events += engine.feed(frame(t + 0.010, []))
    return events


class ArmTests(unittest.TestCase):
    def test_fingers_landing_together_begin_the_swipe(self):
        engine = ThreeFingerSwipe()
        self.assertEqual(land(engine), [])
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.44)
        self.assertEqual(types(events)[0], SwipeBegin)
        self.assertIn(SwipeUpdate, types(events))

    def test_three_finger_tap_never_opens_the_switcher(self):
        engine = ThreeFingerSwipe()
        land(engine)
        hold(engine, 0.02, 0.5, duration=0.05)
        events = release(engine, 0.08, 0.5)
        self.assertEqual(types(events), [SwipeTap])

    def test_a_long_three_finger_hold_is_not_a_tap(self):
        engine = ThreeFingerSwipe()
        land(engine)
        hold(engine, 0.02, 0.5, duration=0.5)  # 500ms, past tap_max_ms
        events = release(engine, 0.55, 0.5)
        self.assertEqual(events, [])

    def test_tiny_wobble_stays_inside_the_dead_zone(self):
        engine = ThreeFingerSwipe()
        land(engine)
        # 0.02 of the pad is under the 0.03 dead zone.
        events, t = drag_slowly(engine, 0.02, 0.5, 0.48, duration=0.2)
        self.assertNotIn(SwipeBegin, types(events))
        events = release(engine, t, 0.48)
        self.assertNotIn(SwipeEnd, types(events))

    def test_late_third_finger_is_a_scroll_not_a_gesture(self):
        engine = ThreeFingerSwipe()
        engine.feed(frame(0.0, [(0.5, 0.5)]))
        engine.feed(frame(0.10, [(0.5, 0.5), (0.5, 0.5)]))
        engine.feed(frame(0.30, [(0.5, 0.5), (0.5, 0.5), (0.5, 0.5)]))
        events, _ = drag_slowly(engine, 0.32, 0.5, 0.35)
        self.assertEqual(events, [])

    def test_two_finger_scroll_is_left_alone(self):
        engine = ThreeFingerSwipe()
        events = []
        for i in range(1, 12):
            x = 0.5 - i * 0.02
            events += engine.feed(frame(i * 0.02, [(x, 0.5), (x, 0.5)]))
        self.assertEqual(events, [])

    def test_one_finger_drag_is_left_alone(self):
        engine = ThreeFingerSwipe()
        events = []
        for i in range(1, 20):
            events += engine.feed(frame(i * 0.01, [(0.5 - i * 0.01, 0.5)]))
        self.assertEqual(events, [])

    def test_vertical_swipe_is_not_ours(self):
        engine = ThreeFingerSwipe()
        land(engine)
        events = []
        for i in range(1, 10):
            events += engine.feed(frame(0.02 + i * 0.02, [(0.5, 0.5 - i * 0.02)] * 3))
        self.assertEqual(events, [])

    def test_a_sequence_can_be_reused_after_the_fingers_lift(self):
        engine = ThreeFingerSwipe()
        land(engine)
        _, t = drag_slowly(engine, 0.02, 0.5, 0.44)
        release(engine, t + 0.2, 0.44)
        events = land(engine, t + 0.4)
        self.assertEqual(events, [])
        events, _ = drag_slowly(engine, t + 0.42, 0.5, 0.44)
        self.assertEqual(types(events)[0], SwipeBegin)


class ProgressTests(unittest.TestCase):
    def test_progress_is_one_step_per_step_fraction(self):
        engine = ThreeFingerSwipe()
        land(engine)
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.5 + STEP, duration=0.6)
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        self.assertAlmostEqual(updates[-1].progress, 1.0, places=3)

    def test_swiping_right_selects_the_next_window(self):
        engine = ThreeFingerSwipe()
        land(engine)
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.6, duration=0.6)
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        self.assertGreater(updates[-1].progress, 0)
        self.assertEqual(updates[-1].steps, 1)

    def test_swiping_left_goes_back(self):
        engine = ThreeFingerSwipe()
        land(engine)
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.4, duration=0.6)
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        self.assertLess(updates[-1].progress, 0)
        self.assertEqual(updates[-1].steps, -1)

    def test_invert_direction_flips_progress(self):
        engine = ThreeFingerSwipe(GestureParams(invert=True))
        land(engine)
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.4, duration=0.6)
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        self.assertGreater(updates[-1].progress, 0)


#: The shipped feel, taken from the config rather than written out again, so
#: changing the default cannot leave these tests asserting the old one.
STEP = GestureParams.from_config(Config()).step_fraction
COMMIT = GestureParams.from_config(Config()).commit_fraction


class CommitTests(unittest.TestCase):
    def _end_after_dragging(self, dx_pad: float):
        engine = ThreeFingerSwipe()
        land(engine)
        target = 0.5 + dx_pad
        events, t = drag_slowly(engine, 0.02, 0.5, target, duration=0.5)
        events += hold(engine, t, target)[0]
        events += release(engine, t + 0.2, target)
        return [e for e in events if isinstance(e, SwipeEnd)]

    def test_past_the_commit_fraction_moves_one_window(self):
        end = self._end_after_dragging((COMMIT + 0.01) * STEP)
        self.assertEqual([e.steps for e in end], [1])

    def test_just_short_of_the_commit_fraction_snaps_back(self):
        end = self._end_after_dragging((COMMIT - 0.01) * STEP)
        self.assertEqual([e.steps for e in end], [0])

    def test_exactly_at_the_commit_fraction_moves_one_window(self):
        end = self._end_after_dragging(COMMIT * STEP)
        self.assertEqual([e.steps for e in end], [1])

    def test_backwards_past_the_commit_fraction_goes_back_one(self):
        end = self._end_after_dragging(-(COMMIT + 0.01) * STEP)
        self.assertEqual([e.steps for e in end], [-1])

    def test_backwards_just_short_snaps_back(self):
        end = self._end_after_dragging(-(COMMIT - 0.01) * STEP)
        self.assertEqual([e.steps for e in end], [0])

    def test_two_full_steps_advance_two_windows(self):
        end = self._end_after_dragging(1.9 * STEP)
        self.assertEqual([e.steps for e in end], [2])

    def test_speed_never_changes_the_commit(self):
        """A quick flick must not switch to a window the outline never showed.

        The outline is driven by ``SwipeUpdate.steps``. If speed could add a
        step at release, you would watch card 0 stay outlined and get card 1 --
        which is exactly what "it is not switching properly" looks like.
        """
        for duration in (0.03, 0.06, 0.12, 0.5):
            with self.subTest(duration=duration):
                engine = ThreeFingerSwipe()
                land(engine)
                target = 0.5 + 0.35 * STEP  # short of the commit fraction
                events, t = drag_slowly(
                    engine, 0.02, 0.5, target, duration=duration, rate=120.0
                )
                events += release(engine, t, target)
                updates = [e for e in events if isinstance(e, SwipeUpdate)]
                end = [e for e in events if isinstance(e, SwipeEnd)]
                self.assertEqual(len(end), 1)
                self.assertEqual(updates[-1].steps, 0, "the outline stayed on card 0")
                self.assertEqual(end[0].steps, updates[-1].steps,
                                 "release must open the card the outline showed")

    def test_same_short_drag_without_speed_snaps_back(self):
        # Short of the commit fraction, so it must not commit no matter how
        # long the fingers rest there. (Expressed in steps, not absolute pad
        # fractions: the shipped step size has changed before and will again.)
        target = 0.5 + (COMMIT - 0.15) * STEP
        engine = ThreeFingerSwipe()
        land(engine)
        events, t = drag_slowly(engine, 0.02, 0.5, target, duration=0.6)
        events += hold(engine, t, target)[0]
        events += release(engine, t + 0.2, target)
        end = [e for e in events if isinstance(e, SwipeEnd)]
        self.assertEqual([e.steps for e in end], [0])

    def test_slow_drag_past_nothing_does_not_advance(self):
        engine = ThreeFingerSwipe()
        land(engine)
        events, t = drag_slowly(engine, 0.02, 0.5, 0.49, duration=0.4)
        events += hold(engine, t, 0.49)[0]
        events += release(engine, t + 0.2, 0.49)
        end = [e for e in events if isinstance(e, SwipeEnd) and e.steps]
        self.assertEqual(end, [])

    def test_all_fingers_lifting_in_one_frame_still_ends(self):
        engine = ThreeFingerSwipe()
        land(engine)
        _, t = drag_slowly(engine, 0.02, 0.5, 0.44)
        events = engine.feed(frame(t + 0.02, []))
        self.assertEqual(types(events), [SwipeEnd])

    def test_flush_ends_a_live_gesture(self):
        engine = ThreeFingerSwipe()
        land(engine)
        drag_slowly(engine, 0.02, 0.5, 0.44)
        self.assertTrue(engine.active)
        events = engine.flush()
        self.assertEqual(types(events), [SwipeEnd])
        self.assertFalse(engine.active)


class CancelTests(unittest.TestCase):
    def test_fourth_finger_cancels(self):
        engine = ThreeFingerSwipe()
        land(engine)
        events, t = drag_slowly(engine, 0.02, 0.5, 0.44)
        self.assertIn(SwipeBegin, types(events))
        events = engine.feed(frame(t + 0.02, [(0.44, 0.5)] * 4))
        self.assertEqual(types(events), [SwipeCancel])
        self.assertFalse(engine.active)

    def test_no_gesture_ever_starts_after_four_fingers(self):
        engine = ThreeFingerSwipe()
        engine.feed(frame(0.0, [(0.5, 0.5)] * 4))
        engine.feed(frame(0.01, [(0.5, 0.5)] * 3))
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.35)
        self.assertEqual(events, [])

    def test_release_after_cancel_does_not_commit(self):
        engine = ThreeFingerSwipe()
        land(engine)
        _, t = drag_slowly(engine, 0.02, 0.5, 0.44)
        engine.feed(frame(t + 0.02, [(0.44, 0.5)] * 4))
        events = release(engine, t + 0.04, 0.44)
        self.assertEqual(events, [])

    def test_dropped_events_cancel_instead_of_jumping(self):
        engine = ThreeFingerSwipe()
        land(engine)
        drag_slowly(engine, 0.02, 0.5, 0.44)
        events = engine.feed(frame(0.5, [], dropped=True))
        self.assertEqual(types(events), [SwipeCancel])
        self.assertEqual(events[0].reason, "syn_dropped")


class StripSelectionTests(unittest.TestCase):
    """The card that is outlined must be the card that opens."""

    def test_shipped_defaults(self):
        config = Config()
        params = GestureParams.from_config(config)
        self.assertAlmostEqual(params.step_fraction, config.step_fraction)
        self.assertAlmostEqual(params.commit_fraction, config.commit_fraction)
        self.assertFalse(params.invert)
        # A card is a deliberate movement, not a haul: a step is a fraction of
        # the pad (0.2 is ~26mm on a 128mm pad).
        self.assertLess(params.step_fraction, 0.2)
        # ...and the strip has to open *before* the outline can tip over, or the
        # first thing a swipe does is select a window nobody asked for.
        self.assertLess(params.dead_zone_fraction, params.commit_fraction * params.step_fraction)

    def test_opening_the_strip_never_selects_another_window(self):
        """The very first update of a swipe still points at the window you are on."""
        config = Config()
        params = GestureParams.from_config(config)
        engine = ThreeFingerSwipe(params)
        land(engine)
        # Stop just past the dead zone, the earliest the strip can appear.
        events, _ = drag_slowly(engine, 0.02, 0.5, 0.5 + config.dead_zone_fraction * 1.01,
                                duration=0.3)
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        self.assertTrue(updates, "a drag past the dead zone must open the strip")
        self.assertEqual(updates[0].steps, 0)
        self.assertLess(updates[0].progress, params.commit_fraction)

    def test_highlight_moves_with_the_fingers(self):
        self.assertEqual(highlight_index(0.6, 1, 5), 2)
        self.assertEqual(highlight_index(-0.6, 3, 5), 2)

    def test_highlight_tips_over_at_half_a_step(self):
        self.assertEqual(highlight_index(0.49, 2, 6), 2)
        self.assertEqual(highlight_index(0.5, 2, 6), 3)
        self.assertEqual(highlight_index(-0.49, 2, 6), 2)
        self.assertEqual(highlight_index(-0.5, 2, 6), 1)

    def test_highlight_clamps_at_the_ends(self):
        self.assertEqual(highlight_index(9.0, 4, 5), 4)
        self.assertEqual(highlight_index(-9.0, 0, 5), 0)
        self.assertEqual(highlight_index(1.0, 0, 0), 0)

    def test_highlight_matches_the_commit_rule(self):
        """Pin the two together: they must never disagree.

        ``highlight_index`` is only used when there is no engine event to hand,
        so it has to round exactly like the engine's own preview does.
        """
        params = GestureParams.from_config(Config())
        engine = ThreeFingerSwipe(params)
        middle = 50  # far from either end, so clamping cannot mask a mismatch
        for hundredths in range(-250, 251, 7):
            progress = hundredths / 100.0
            engine.progress = progress
            expected = engine.preview_steps
            self.assertEqual(
                highlight_index(progress, middle, 99) - middle, expected,
                f"progress {progress}: highlight and commit disagree",
            )

    def test_update_selection_at_a_distance(self):
        config = Config()
        engine = ThreeFingerSwipe(GestureParams.from_config(config))
        land(engine)  # fingers land at 0.5, which is the anchor progress is measured from
        events, _ = drag_slowly(
            engine, 0.02, 0.5, 0.5 + 1.4 * config.step_fraction, duration=0.8
        )
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        self.assertAlmostEqual(updates[-1].progress, 1.4, places=2)
        self.assertEqual(updates[-1].steps, 1, "1.4 steps points at the second window, not the third")

    def test_end_selection_and_update_selection_agree(self):
        config = Config()
        engine = ThreeFingerSwipe(GestureParams.from_config(config))
        land(engine)
        target = 0.5 + 1.0 * config.step_fraction
        events, t = drag_slowly(engine, 0.02, 0.5, target, duration=0.8)
        events += hold(engine, t, target)[0]
        events += release(engine, t + 0.2, target)
        updates = [e for e in events if isinstance(e, SwipeUpdate)]
        ends = [e for e in events if isinstance(e, SwipeEnd)]
        self.assertEqual(len(ends), 1)
        self.assertEqual(ends[0].steps, updates[-1].steps)


class RubberBandTests(unittest.TestCase):
    def test_inside_the_range_is_untouched(self):
        self.assertAlmostEqual(apply_rubber_band(1.5, 0, 4), 1.5)

    def test_past_the_end_is_compressed(self):
        # index 3 of 4 apps: nothing beyond it, so all overshoot is squeezed.
        self.assertAlmostEqual(apply_rubber_band(5.0, 3, 4), 1.75)
        # index 0 of 4: up to 3 steps are free, the rest is squeezed.
        self.assertAlmostEqual(apply_rubber_band(5.0, 0, 4), 3.0 + 2.0 * 0.35)

    def test_past_the_start_is_compressed(self):
        # index 2 of 4 apps: 2 steps back are free, the third is squeezed.
        self.assertAlmostEqual(apply_rubber_band(-3.0, 2, 4), -2.35)

    def test_single_app_cannot_move(self):
        self.assertAlmostEqual(apply_rubber_band(3.0, 0, 1), 3.0 * 0.35)

    def test_clamp_keeps_steps_inside_the_list(self):
        self.assertEqual(clamp_steps(5, 0, 3), 2)
        self.assertEqual(clamp_steps(-5, 2, 3), -2)
        self.assertEqual(clamp_steps(1, 1, 3), 1)
        self.assertEqual(clamp_steps(1, 0, 0), 0)


class ReplayConsistencyTests(unittest.TestCase):
    """A recorded swipe should replay to exactly the same decision."""

    def test_same_input_same_decision(self):
        def run():
            engine = ThreeFingerSwipe()
            land(engine)
            events, t = drag_slowly(engine, 0.02, 0.5, 0.42, duration=0.35)
            events += hold(engine, t, 0.42)[0]
            events += release(engine, t + 0.2, 0.42)
            return [e.steps for e in events if isinstance(e, SwipeEnd)]

        self.assertEqual(run(), run())


if __name__ == "__main__":
    unittest.main()
