"""The three-finger horizontal swipe state machine.

This module is deliberately pure: it takes ContactFrames and returns gesture
events. No X11, no GTK, no threads. That keeps the trickiest logic in the
project testable with synthetic event streams instead of hands on a touchpad.

The rules mimic how Windows 11 accepts a three-finger swipe:

* all three fingers must land within a short window, and never four
* movement is ignored until it passes a small dead zone, so taps and jitter do
  nothing
* the movement must be clearly horizontal; anything vertical is left alone
* the switcher opens on the first real movement and then follows the fingers
* **moving right selects the next window to the right** (``invert`` swaps it)
* on release you commit to the window you have moved at least
  ``commit_fraction`` past -- speed never changes the answer, so the card that
  is outlined while you drag is *always* the card that opens when you let go

``SwipeUpdate`` and ``SwipeEnd`` both carry the window the progress points at,
computed by the very same pure rule, so the outline and the commit cannot
drift apart at any speed.
"""

from __future__ import annotations

import dataclasses
import math

from .touchpad import ContactFrame


@dataclasses.dataclass(frozen=True)
class GestureParams:
    """Tuning for the swipe, all distances as fractions of the pad width."""

    #: These mirror ``config.Config``'s defaults so a bare GestureParams feels
    #: like the shipped app; the config wins when there is one.
    step_fraction: float = 0.095
    dead_zone_fraction: float = 0.025
    direction_dominance: float = 1.15
    commit_fraction: float = 0.5
    arm_window: float = 0.040
    tap_max_ms: float = 300.0
    tap_max_move: float = 0.015
    invert: bool = False
    velocity_window: float = 0.080

    @classmethod
    def from_config(cls, cfg) -> GestureParams:
        return cls(
            step_fraction=cfg.step_fraction,
            dead_zone_fraction=cfg.dead_zone_fraction,
            direction_dominance=cfg.direction_dominance,
            commit_fraction=cfg.commit_fraction,
            arm_window=cfg.arm_window_ms / 1000.0,
            tap_max_ms=cfg.tap_max_ms,
            tap_max_move=cfg.tap_max_move,
            invert=cfg.invert_direction,
        )


# --- events ---------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class SwipeBegin:
    time: float


@dataclasses.dataclass(frozen=True)
class SwipeUpdate:
    """Progress in window steps, plus the window it currently points at.

    ``steps`` is how many windows away the selection is right now -- it is the
    index the strip should be outlining, and the value a release would commit
    (before any flick bonus). +1.0 progress means "one card to the right",
    i.e. one step further along the row.
    """

    progress: float
    time: float
    steps: int = 0


@dataclasses.dataclass(frozen=True)
class SwipeEnd:
    progress: float
    velocity: float
    steps: int
    time: float


@dataclasses.dataclass(frozen=True)
class SwipeCancel:
    reason: str
    time: float


@dataclasses.dataclass(frozen=True)
class SwipeTap:
    time: float


GestureEvent = SwipeBegin | SwipeUpdate | SwipeEnd | SwipeCancel | SwipeTap


# --- state machine --------------------------------------------------------

IDLE = "idle"
LANDING = "landing"
DECIDING = "deciding"
TRACKING = "tracking"
DONE = "done"
BLOCKED = "blocked"


class ThreeFingerSwipe:
    """Feed it frames, get gesture events."""

    def __init__(self, params: GestureParams | None = None):
        self.params = params or GestureParams()
        self.state: str = IDLE
        self.progress: float = 0.0
        self._seq_t0: float = 0.0
        self._anchor: tuple[float, float] = (0.0, 0.0)
        self._max_move: float = 0.0
        self._samples: list[tuple[float, float]] = []
        self._last_frame: float = 0.0
        self._tap_ok: bool = False

    # -- public API --------------------------------------------------------
    def feed(self, frame: ContactFrame) -> list[GestureEvent]:
        events: list[GestureEvent] = []
        count = frame.count
        self._last_frame = frame.time

        if frame.dropped:
            if self.state == TRACKING:
                events.append(SwipeCancel("syn_dropped", frame.time))
            self._reset()
            return events

        if count == 0:
            if self.state == TRACKING:
                # All three fingers lifted inside one report: still a release.
                events.extend(self._finish(frame.time))
            elif self._tap_ok and self._is_tap(frame.time):
                events.append(SwipeTap(frame.time))
            self._reset()
            return events

        # Four or more fingers: this touch sequence is not a three-finger swipe.
        if count > 3:
            if self.state == TRACKING:
                events.append(SwipeCancel("extra_finger", frame.time))
                self.state = BLOCKED
                self._samples.clear()
            elif self.state != BLOCKED:
                self.state = BLOCKED
            self._tap_ok = False
            return events

        if self.state == BLOCKED:
            return events

        if self.state == IDLE:
            self._seq_t0 = frame.time
            self.state = LANDING

        if self.state == LANDING:
            return events + self._landing(frame)

        if self.state == DECIDING:
            return events + self._deciding(frame)

        if self.state == TRACKING:
            return events + self._tracking(frame)

        return events  # DONE: wait for the fingers to lift

    def flush(self, time: float | None = None) -> list[GestureEvent]:
        """Called when input stops (device loss, shutdown).

        Ends a live gesture safely instead of leaving the overlay on screen.
        """
        if self.state == TRACKING:
            events = self._finish(time if time is not None else self._last_frame)
            self.state = DONE
            return events
        self._reset()
        return []

    @property
    def active(self) -> bool:
        return self.state == TRACKING

    # -- states ------------------------------------------------------------
    def _landing(self, frame: ContactFrame) -> list[GestureEvent]:
        elapsed = frame.time - self._seq_t0
        if frame.count == 3 and elapsed <= self.params.arm_window:
            centroid = frame.centroid or (0.0, 0.0)
            self._anchor = centroid
            self._max_move = 0.0
            self._tap_ok = True
            self.state = DECIDING
        elif elapsed > self.params.arm_window:
            # The fingers did not land together: a scroll with a stray finger.
            self._tap_ok = False
            self.state = BLOCKED
        return []

    def _deciding(self, frame: ContactFrame) -> list[GestureEvent]:
        if frame.count != 3:
            # A finger lifted before we moved anywhere. Stay put: the frames
            # that follow either end the sequence (tap) or drift (ignored).
            return []

        dx, dy = self._delta(frame)
        self._max_move = max(self._max_move, math.hypot(dx, dy))
        distance = math.hypot(dx, dy)
        if distance < self.params.dead_zone_fraction:
            return []

        if abs(dx) < self.params.direction_dominance * abs(dy):
            # Vertical or ambiguous movement: not this gesture.
            self._tap_ok = False
            self.state = BLOCKED
            return []

        self._tap_ok = False
        self.state = TRACKING
        self.progress = self._progress(dx)
        self._samples = [(frame.time, self.progress)]
        return [
            SwipeBegin(frame.time),
            SwipeUpdate(self.progress, frame.time, self.preview_steps),
        ]

    def _tracking(self, frame: ContactFrame) -> list[GestureEvent]:
        if frame.count != 3:
            events = self._finish(frame.time)
            self.state = DONE
            return events

        dx, _ = self._delta(frame)
        self._max_move = max(self._max_move, abs(dx))
        self.progress = self._progress(dx)
        self._samples.append((frame.time, self.progress))
        if len(self._samples) > 64:
            del self._samples[:-32]
        return [SwipeUpdate(self.progress, frame.time, self.preview_steps)]

    # -- helpers -----------------------------------------------------------
    @property
    def preview_steps(self) -> int:
        """The window this progress points at: exactly what a release would pick.

        Computed with ``_commit_steps`` itself, so the highlighted card and the
        released card can never drift apart -- including the half-step boundary
        and any custom ``commit_fraction``.
        """
        return self._commit_steps(self.progress)

    def _finish(self, time: float) -> list[GestureEvent]:
        # Velocity is reported for the log only. It deliberately does not
        # decide anything: a swipe that commits a window the outline never
        # showed is the one thing a strip of windows must never do.
        velocity = self._velocity()
        return [SwipeEnd(self.progress, velocity, self.preview_steps, time)]

    def _delta(self, frame: ContactFrame) -> tuple[float, float]:
        centroid = frame.centroid or self._anchor
        return (centroid[0] - self._anchor[0], centroid[1] - self._anchor[1])

    def _progress(self, dx: float) -> float:
        # Moving right (dx > 0) steps forward through the list, which is the
        # window to the right on the carousel.
        sign = -1.0 if self.params.invert else 1.0
        return sign * dx / max(1e-6, self.params.step_fraction)

    def _velocity(self) -> float:
        """Steps per second over the last few frames."""
        window = self.params.velocity_window
        if len(self._samples) < 2:
            return 0.0
        last_t, last_p = self._samples[-1]
        first_t, first_p = self._samples[0]
        for sample_t, sample_p in reversed(self._samples[:-1]):
            if last_t - sample_t >= window:
                first_t, first_p = sample_t, sample_p
                break
        dt = last_t - first_t
        if dt <= 1e-6:
            return 0.0
        return (last_p - first_p) / dt

    def _commit_steps(self, progress: float) -> int:
        """How many windows along the progress has travelled.

        The only rule: half a step past the current card tips the selection to
        the next one. It is pure and takes no other input, which is what makes
        ``SwipeUpdate.steps`` and ``SwipeEnd.steps`` the same number by
        construction rather than by agreement.
        """
        return _steps_at(self.progress, self.params.commit_fraction)

    def _is_tap(self, time: float) -> bool:
        duration = (time - self._seq_t0) * 1000.0
        return duration <= self.params.tap_max_ms and self._max_move <= self.params.tap_max_move

    def _reset(self) -> None:
        self.state = IDLE
        self.progress = 0.0
        self._max_move = 0.0
        self._tap_ok = False
        self._samples.clear()

    def describe(self) -> str:
        return f"state={self.state} progress={self.progress:+.3f}"


# --- display helpers ------------------------------------------------------


#: Slack added before the floor/ceil, so a progress that lands exactly on the
#: commit fraction counts as past it. Without it, 0.5 steps can round down on
#: the last bit of a float and half a step would not reliably tip the card over.
_BOUNDARY_EPSILON = 1e-9


def _steps_at(progress: float, commit_fraction: float) -> int:
    """The one commit rule: which card away this progress points at.

    Shared by the engine and by ``highlight_index`` so the outlined card and
    the opened card are the same card by construction, not by convention.
    """
    if progress >= 0:
        return math.floor(progress + 1.0 - commit_fraction + _BOUNDARY_EPSILON)
    return math.ceil(progress - 1.0 + commit_fraction - _BOUNDARY_EPSILON)


def highlight_index(progress: float, index: int, count: int, commit_fraction: float = 0.5) -> int:
    """Which window is outlined at this progress, as an absolute index.

    Only needed when there is no engine event to hand (a scripted carousel, or
    a progress value replayed from a recording); the live path uses the
    ``steps`` the engine already put on ``SwipeUpdate``. It shares the engine's
    rule through ``_steps_at``, and ``test_highlight_matches_the_commit_rule``
    pins the two together.
    """
    if count <= 0:
        return 0
    return max(0, min(count - 1, index + _steps_at(progress, commit_fraction)))


def apply_rubber_band(progress: float, index: int, count: int, resistance: float = 0.35) -> float:
    """Compress progress past the first/last app instead of looping.

    `progress` is in app steps from `index`; returns the value to draw so the
    carousel stretches a little at the ends and springs back, like Windows 11.
    """
    if count <= 1:
        return progress * resistance
    low = -float(index)
    high = float(count - 1 - index)
    if progress > high:
        return high + (progress - high) * resistance
    if progress < low:
        return low + (progress - low) * resistance
    return progress


def clamp_steps(steps: int, index: int, count: int) -> int:
    """Keep a committed step inside the list of apps."""
    if count <= 0:
        return 0
    return max(-index, min(count - 1 - index, steps))
