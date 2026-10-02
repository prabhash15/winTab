"""Unit tests for the capture plumbing (no X server, no images needed).

The pixels themselves are the spike's job; what is testable here is the part
that decides *which workspace a grab belongs to* -- the difference between a
card showing the window you asked for and a card showing the workspace you just
left, forever, because nothing on screen happened to change afterwards.
"""

from __future__ import annotations

import pathlib
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from Xlib.error import XError  # noqa: E402

from wintab.capture import ScreenCache, read_current_desktop, stamped_desktop  # noqa: E402
from wintab.config import Config  # noqa: E402


class _Root:
    """A root window that answers one property, or refuses."""

    def __init__(self, value=None, failure: Exception | None = None):
        self.value = value
        self.failure = failure
        self.asked: list = []

    def get_full_property(self, atom, _type):
        self.asked.append(atom)
        if self.failure is not None:
            raise self.failure
        if self.value is None:
            return None
        return SimpleNamespace(value=self.value)


class _Display:
    def __init__(self, atom=42):
        self.atom = atom

    def intern_atom(self, _name):
        if self.atom is None:
            raise AttributeError("no atoms on this display")
        return self.atom


class CurrentDesktopTests(unittest.TestCase):
    """The stamp on a grab has to come from the server, not from the request."""

    def test_the_servers_answer_is_used(self):
        root = _Root([3])
        self.assertEqual(read_current_desktop(_Display(), root), 3)
        self.assertEqual(len(root.asked), 1, "exactly one property read")

    def test_a_missing_property_is_unknown(self):
        self.assertEqual(read_current_desktop(_Display(), _Root(None)), -1)

    def test_an_empty_property_is_unknown(self):
        self.assertEqual(read_current_desktop(_Display(), _Root([])), -1)

    def test_an_x_error_is_unknown_not_a_crash(self):
        root = _Root(failure=XError(None, b"\x00" * 32))
        self.assertEqual(read_current_desktop(_Display(), root), -1)

    def test_a_display_that_cannot_intern_atoms_is_unknown(self):
        self.assertEqual(read_current_desktop(_Display(atom=None), _Root([1])), -1)

    def test_a_nonsense_value_is_unknown(self):
        self.assertEqual(read_current_desktop(_Display(), _Root(["two"])), -1)


class StampedDesktopTests(unittest.TestCase):
    def test_the_observed_workspace_wins(self):
        # Asked for 2, the screen was still showing 1: the grab is a picture of
        # 1, and saying otherwise is what let it be cropped as workspace 2.
        self.assertEqual(stamped_desktop(1, 2), 1)

    def test_without_an_answer_the_request_stands(self):
        self.assertEqual(stamped_desktop(-1, 2), 2)


class RefreshRequestTests(unittest.TestCase):
    """Every ask for fresh pixels has to be countable: the strip's freshness is
    invisible from outside, and "did we ever ask again?" is the first question
    when cards look old."""

    def setUp(self):
        self.cache = ScreenCache(Config())
        self.addCleanup(self.cache.stop)

    def test_requests_are_counted(self):
        self.assertEqual(self.cache.stats["requests"], 0)
        self.cache.refresh_soon()
        self.cache.refresh_soon(urgent=True)
        self.assertEqual(self.cache.stats["requests"], 2)

    def test_only_an_urgent_request_skips_the_settle_wait(self):
        self.cache.refresh_soon()
        self.assertFalse(self.cache._urgent.is_set())
        self.cache.refresh_soon(urgent=True)
        self.assertTrue(self.cache._urgent.is_set())

    def test_the_desktop_asked_for_is_remembered(self):
        self.cache.set_targets([], desktop=2)
        self.assertEqual(self.cache._desktop, 2)
        self.assertEqual(self.cache.snapshot_desktop, -1, "nothing captured yet")


class BusyTests(unittest.TestCase):
    """A swipe must not share the machine with the capture worker.

    The app asks ``busy`` before baking cards and the worker checks it between
    window reads, so the flag has to be readable from both sides.
    """

    def setUp(self):
        self.cache = ScreenCache(Config())
        self.addCleanup(self.cache.stop)

    def test_busy_tracks_the_gesture(self):
        self.assertFalse(self.cache.busy)
        self.cache.set_busy(True)
        self.assertTrue(self.cache.busy)
        self.cache.set_busy(False)
        self.assertFalse(self.cache.busy)

    def test_starting_twice_does_not_start_a_second_thread(self):
        # Two threads on one python-xlib display cross each other's replies, and
        # the failure is a "Request reply to unknown request" crash, not a
        # slow-down -- so this is pinned here rather than discovered later.
        self.cache.start()
        first = self.cache._thread
        self.cache.start()
        self.assertIs(self.cache._thread, first)

    def test_the_last_damage_time_is_readable(self):
        # The app's idle poll uses it to decide whether re-reading the window
        # list could possibly find anything new.
        self.assertEqual(self.cache.last_damage_at, 0.0)
        self.cache._note_damage()
        self.assertGreater(self.cache.last_damage_at, 0.0)


if __name__ == "__main__":
    unittest.main()
