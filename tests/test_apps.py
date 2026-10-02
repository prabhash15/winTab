"""Unit tests for the window/app model (no X server needed)."""

from __future__ import annotations

import pathlib
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from types import SimpleNamespace  # noqa: E402

from Xlib import X, Xatom  # noqa: E402
from Xlib.error import XError  # noqa: E402

from wintab.app import WinTab  # noqa: E402
from wintab.config import Config  # noqa: E402
from wintab.apps import (  # noqa: E402
    WINDOW_CACHE_S,
    MruTracker,
    WindowManager,
    WinInfo,
    build_app_list,
    entry_index,
    frame_of,
    make_sticky,
    rects_overlap,
    root_position,
    strip_priority,
    usable_windows,
    windows_needing_own_pixels,
)


def win(xid: int, wm_class: str = "brave-browser", **kwargs) -> WinInfo:
    defaults = dict(x=0, y=0, width=800, height=600, desktop=0)
    defaults.update(kwargs)
    return WinInfo(xid=xid, wm_class=wm_class, **defaults)


class FilterTests(unittest.TestCase):
    def test_only_windows_from_this_workspace(self):
        windows = [win(1, desktop=0), win(2, desktop=1), win(3, desktop=2)]
        kept = usable_windows(windows, current_desktop=0)
        self.assertEqual([w.xid for w in kept], [1])

    def test_sticky_windows_belong_everywhere(self):
        windows = [win(1, desktop=-1), win(2, desktop=3)]
        kept = usable_windows(windows, current_desktop=3)
        self.assertEqual(sorted(w.xid for w in kept), [1, 2])

    def test_desktop_icon_window_is_never_listed(self):
        windows = [win(1, wm_class="nemo-desktop", desktop_window=True), win(2)]
        kept = usable_windows(windows, current_desktop=0)
        self.assertEqual([w.xid for w in kept], [2])

    def test_panels_and_docks_are_never_listed(self):
        windows = [win(1, wm_class="cinnamon", dock=True), win(2)]
        kept = usable_windows(windows, current_desktop=0)
        self.assertEqual([w.xid for w in kept], [2])

    def test_skip_taskbar_windows_are_never_listed(self):
        windows = [win(1, skip_taskbar=True), win(2)]
        kept = usable_windows(windows, current_desktop=0)
        self.assertEqual([w.xid for w in kept], [2])

    def test_our_own_overlay_is_never_listed(self):
        windows = [win(1, pid=4321), win(2, pid=9999, our_own=True)]
        kept = usable_windows(windows, current_desktop=0, our_pid=4321)
        self.assertEqual([w.xid for w in kept], [])

    def test_minimized_windows_stay_in_the_list(self):
        windows = [win(1, minimized=True)]
        self.assertEqual(len(usable_windows(windows, 0)), 1)

    def test_non_normal_windows_are_dropped(self):
        windows = [win(1, normal=False), win(2)]
        self.assertEqual([w.xid for w in usable_windows(windows, 0)], [2])


class GroupingTests(unittest.TestCase):
    def test_windows_of_one_app_become_a_single_entry(self):
        windows = [win(1), win(2), win(3, wm_class="cinnamon-settings")]
        apps = build_app_list(windows, [1, 2, 3], current_desktop=0)
        self.assertEqual(len(apps), 2)
        self.assertEqual(apps[0].key, "brave-browser")
        self.assertEqual(len(apps[0].windows), 2)

    def test_per_window_mode_keeps_them_apart(self):
        windows = [win(1), win(2)]
        apps = build_app_list(windows, [1, 2], current_desktop=0, group_by_app=False)
        self.assertEqual(len(apps), 2)
        self.assertEqual([a.windows[0].xid for a in apps], [1, 2])

    def test_windows_without_a_class_group_by_pid(self):
        windows = [win(1, wm_class="", pid=5), win(2, wm_class="", pid=5), win(3, wm_class="", pid=6)]
        apps = build_app_list(windows, [1, 2, 3], current_desktop=0)
        self.assertEqual(len(apps), 2)
        self.assertEqual(apps[0].key, "pid:5")

    def test_apps_from_other_workspaces_disappear_entirely(self):
        windows = [win(1, desktop=0), win(2, wm_class="brave-browser", desktop=1)]
        apps = build_app_list(windows, [2, 1], current_desktop=0)
        self.assertEqual([a.key for a in apps], ["brave-browser"])
        self.assertEqual([w.xid for w in apps[0].windows], [1])


class OrderingTests(unittest.TestCase):
    def test_most_recently_used_first(self):
        windows = [win(1), win(2, wm_class="cinnamon-settings"), win(3, wm_class="nemo")]
        apps = build_app_list(windows, [3, 2, 1], current_desktop=0)
        self.assertEqual([a.key for a in apps], ["nemo", "cinnamon-settings", "brave-browser"])

    def test_group_rank_follows_its_best_window(self):
        windows = [win(1), win(2), win(3, wm_class="nemo")]
        # The second Brave window is the most recent, so Brave leads.
        apps = build_app_list(windows, [2, 3, 1], current_desktop=0)
        self.assertEqual([a.key for a in apps], ["brave-browser", "nemo"])

    def test_windows_inside_a_group_follow_mru_too(self):
        windows = [win(1), win(2), win(3)]
        apps = build_app_list(windows, [2, 1, 3], current_desktop=0)
        self.assertEqual([w.xid for w in apps[0].windows], [2, 1, 3])

    def test_windows_missing_from_the_mru_go_last(self):
        windows = [win(1), win(2), win(3)]
        apps = build_app_list(windows, [3], current_desktop=0)
        self.assertEqual(apps[0].windows[0].xid, 3)

    def test_entry_label_is_readable(self):
        windows = [win(1, wm_class="cinnamon-settings gestures")]
        apps = build_app_list(windows, [1], current_desktop=0)
        self.assertEqual(apps[0].label, "Cinnamon Settings Gestures")


class StripOrderTests(unittest.TestCase):
    """The row's left-to-right order, and which card the outline starts on."""

    def test_opened_mode_uses_the_mapping_order(self):
        # _NET_CLIENT_LIST is "order opened", so it wins over focus history.
        self.assertEqual(strip_priority("opened", [3, 2, 1], [1, 2, 3]), [1, 2, 3])

    def test_recent_mode_uses_the_mru(self):
        self.assertEqual(strip_priority("recent", [3, 2, 1], [1, 2, 3]), [3, 2, 1])

    def test_unknown_mode_falls_back_to_the_stable_row(self):
        self.assertEqual(strip_priority("nonsense", [3, 2, 1], [1, 2, 3]), [1, 2, 3])

    def test_first_mention_wins_when_a_window_appears_twice(self):
        # The appended tail is the stacking order. A plain dict comprehension
        # took the *last* mention, which parked the focused window at the far
        # right of every strip; the first mention has to win.
        windows = [win(1), win(2), win(3)]
        apps = build_app_list(windows, [3, 1, 2, 1, 2, 3], current_desktop=0, group_by_app=False)
        self.assertEqual([a.windows[0].xid for a in apps], [3, 1, 2])

    def test_row_does_not_move_when_focus_changes(self):
        # Focus history in, focus history out: with "opened" the row is the
        # same before and after a switch, only the outline moves.
        windows = [win(1), win(2), win(3)]
        mapping = [1, 2, 3]
        before = build_app_list(
            windows, strip_priority("opened", [3, 2, 1], mapping) + mapping, 0, group_by_app=False
        )
        after = build_app_list(
            windows, strip_priority("opened", [1, 3, 2], mapping) + mapping, 0, group_by_app=False
        )
        self.assertEqual([a.windows[0].xid for a in before], mapping)
        self.assertEqual(
            [a.windows[0].xid for a in before], [a.windows[0].xid for a in after]
        )
        self.assertEqual(entry_index(before, 3), 2)
        self.assertEqual(entry_index(after, 1), 0)

    def test_focused_window_is_not_pinned_to_the_end(self):
        # With a stable row, focusing the *first* window must outline the first
        # card -- the old MRU ordering always outlined the rightmost one, so
        # every rightward swipe hit the end stop.
        windows = [win(1), win(2), win(3)]
        apps = build_app_list(windows, [1, 2, 3] + [1, 2, 3], 0, group_by_app=False)
        self.assertEqual(entry_index(apps, 1), 0)
        self.assertEqual(entry_index(apps, 2), 1)
        self.assertEqual(entry_index(apps, 3), 2)

    def test_entry_index_falls_back_to_the_first_card(self):
        windows = [win(1)]
        apps = build_app_list(windows, [1], 0, group_by_app=False)
        self.assertEqual(entry_index(apps, None), 0)
        self.assertEqual(entry_index(apps, 4242), 0)

    def test_entry_index_finds_a_window_inside_a_group(self):
        windows = [win(1, wm_class="nemo"), win(2), win(3)]
        apps = build_app_list(windows, [1, 2, 3], 0)
        # Brave (2 and 3) is one card, so its second window is still card 1.
        self.assertEqual(entry_index(apps, 3), 1)


class WindowCacheTests(unittest.TestCase):
    """Enumerating windows is a round trip per property per window, on the GTK
    thread, and opening the strip used to do it twice in a row."""

    def setUp(self):
        self._calls = 0

    def _manager(self):
        manager = object.__new__(WindowManager)
        manager._windows = [win(1), win(2)]
        manager._windows_at = time.monotonic()
        manager._static = {1: ("nemo", 7)}
        manager.stacking_order = lambda: [1, 2]
        manager._describe = self._describe
        return manager

    def _describe(self, xid):
        self._calls += 1
        return win(xid)

    def test_a_second_read_inside_the_window_is_free(self):
        manager = self._manager()
        first = manager.list_windows()
        manager._describe = lambda xid: (_ for _ in ()).throw(AssertionError("re-read"))
        self.assertEqual([w.xid for w in manager.list_windows()], [1, 2])
        self.assertEqual([w.xid for w in first], [1, 2])

    def test_force_always_re_reads(self):
        manager = self._manager()
        manager.list_windows(force=True)
        self.assertEqual(self._calls, 2)
        manager.list_windows(force=True)
        self.assertEqual(self._calls, 4)

    def test_an_invalidated_cache_re_reads(self):
        manager = self._manager()
        manager.list_windows()
        manager.invalidate()
        manager.list_windows()
        self.assertEqual(self._calls, 2)

    def test_a_stale_cache_re_reads(self):
        manager = self._manager()
        manager._windows_at = time.monotonic() - WINDOW_CACHE_S - 1.0
        manager.list_windows()
        self.assertEqual(self._calls, 2)

    def test_the_returned_list_cannot_be_edited_by_the_caller(self):
        manager = self._manager()
        windows = manager.list_windows()
        windows.clear()
        self.assertEqual(len(manager.list_windows()), 2, "the cache must survive")

    def test_a_gone_window_is_forgotten_so_its_facts_cannot_be_reused(self):
        manager = self._manager()
        manager._describe = lambda xid: win(1) if xid == 1 else None
        manager.list_windows(force=True)
        self.assertEqual(list(manager._static), [1])


class _Reply:
    """python-xlib's TranslateCoords reply: fields are x and y."""

    def __init__(self, x: int, y: int):
        self.x, self.y = x, y


class _LegacyReply:
    """The names an older release of the library used, to prove we do not
    silently accept 'no answer' as a position."""

    dst_x = 1
    dst_y = 2


class _Root:
    id = 0x4AE

    def __init__(self, reply=None):
        self.reply = reply if reply is not None else _Reply(1031, 89)
        self.asked: list = []

    def translate_coords(self, window, x, y):
        self.asked.append((window, x, y))
        return self.reply


class _BrokenRoot(_Root):
    """A connection where the request cannot be sent at all."""

    def translate_coords(self, window, x, y):
        raise AttributeError("translate_coords")


class _Window:
    id = 0x5C00009

    def __init__(self, parent=None, reply=None):
        self._parent = parent
        self.reply = reply
        self.translated_of = []
        self.got_geometry = False

    def translate_coords(self, window, x, y):
        # A window would answer in its *parent's* coordinates: the wrong call.
        self.translated_of.append((window, x, y))
        return self.reply if self.reply is not None else _Reply(10, 40)

    def query_tree(self):
        if self._parent == "broken":
            raise XError(None, b"\x00" * 32)
        return SimpleNamespace(parent=self._parent)


class OwnPixelTests(unittest.TestCase):
    """Reading a window from the compositor costs ~23ms, so only do it when a
    crop of the screen would show the wrong thing."""

    SCREEN = (0, 0, 1920, 1080)

    def test_overlapping_rects(self):
        self.assertTrue(rects_overlap((0, 0, 10, 10), (5, 5, 10, 10)))
        self.assertFalse(rects_overlap((0, 0, 10, 10), (10, 0, 10, 10)))
        self.assertFalse(rects_overlap((0, 0, 10, 10), (0, 20, 10, 10)))

    def test_nothing_overlaps_means_a_crop_is_enough(self):
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, x=500, y=0, width=400, height=300)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), set())

    def test_a_covered_window_needs_its_own_pixels(self):
        # Bottom to top: window 1 is behind 2, and they overlap.
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, x=100, y=100, width=400, height=300)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), {1})

    def test_the_front_window_still_gets_away_with_a_crop(self):
        # Three windows in a cascade; only the frontmost one is uncropped by
        # anything, so the other two need reading from the compositor.
        windows = [win(1, x=0, y=0, width=300, height=300),
                   win(2, x=50, y=50, width=300, height=300),
                   win(3, x=100, y=100, width=300, height=300)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), {1, 2})

    def test_touching_edges_are_not_overlapping(self):
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, x=400, y=0, width=400, height=300)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), set())

    def test_a_window_hanging_off_the_screen_needs_its_own_pixels(self):
        windows = [win(1, x=1800, y=0, width=400, height=300)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), {1})

    def test_windows_on_other_workspaces_do_not_cover_anything(self):
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, x=0, y=0, width=400, height=300, desktop=3)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), set())

    def test_a_minimized_window_covers_nothing(self):
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, x=0, y=0, width=400, height=300, minimized=True)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), set())

    def test_our_own_overlay_never_covers_anything(self):
        # The overlay is a full-screen window and is always on top: if the
        # stacking test counted it, every card would take the slow path.
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, our_own=True, x=0, y=0, width=1920, height=1080)]
        self.assertEqual(
            windows_needing_own_pixels(windows, 0, self.SCREEN, our_pid=None), set()
        )

    def test_a_panel_above_a_window_really_does_cover_it(self):
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, wm_class="cinnamon", dock=True, x=0, y=0, width=1920, height=40)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), {1})

    def test_another_process_window_above_still_counts(self):
        windows = [win(1, x=0, y=0, width=400, height=300),
                   win(2, pid=999, x=0, y=0, width=400, height=300)]
        self.assertEqual(
            windows_needing_own_pixels(windows, 0, self.SCREEN, our_pid=999), set()
        )

    def test_sticky_windows_are_compared_too(self):
        windows = [win(1, x=0, y=0, width=400, height=300, desktop=-1),
                   win(2, x=0, y=0, width=400, height=300, desktop=0)]
        self.assertEqual(windows_needing_own_pixels(windows, 0, self.SCREEN), {1})


class _FakeWindowManager:
    """Just enough of WindowManager for the app's publish step."""

    def __init__(self, windows, desktop=0):
        self._windows = list(windows)
        self._desktop = desktop
        self._own_pid = 4242
        self.screen = (0, 0, 1920, 1080)
        self.mru = MruTracker()
        self.activated: list[int] = []
        self.refreshed = 0

    def activate(self, xid):
        self.activated.append(int(xid))

    def active_window(self):
        return self.activated[-1] if self.activated else None

    def refresh_mru(self):
        self.refreshed += 1

    def list_windows(self, force=False):
        # The real one caches and takes `force`; the app asks with force=True
        # when noticing that a window moved is the point of the pass.
        self.forces = getattr(self, "forces", 0) + (1 if force else 0)
        return list(self._windows)

    def current_desktop(self):
        return self._desktop

    def screen_geometry(self):
        return self.screen


class _FakeCache:
    def __init__(self):
        self.targets = []
        self.desktop = None
        self.urgencies = 0

    def set_targets(self, targets, desktop=-1):
        self.targets = list(targets)
        self.desktop = desktop

    def refresh_soon(self, urgent=False):
        if urgent:
            self.urgencies += 1

    def set_busy(self, busy):
        self.busy = busy


class PublishTargetsTests(unittest.TestCase):
    """What the app asks the capture thread to read, and what it may crop.

    Getting this wrong is invisible in the log and only shows up as cards of the
    wrong window after a workspace switch, so it is pinned here: every window on
    screen is asked for its *own* pixels, and a crop is offered only where a
    crop would be right anyway.
    """

    def _publish(self, windows, desktop=0):
        app = WinTab(Config(), use_overlay=False)
        app.wm = _FakeWindowManager(windows, desktop)
        app.cache = _FakeCache()
        app._refresh_rects()
        return app

    def test_every_placed_window_is_asked_for_its_own_pixels(self):
        # A cascade plus one on its own: the covered ones used to be the only
        # targets, and everything else was cropped out of a screen grab.
        windows = [win(1, x=0, y=0, width=300, height=300),
                   win(2, x=50, y=50, width=300, height=300),
                   win(3, x=900, y=0, width=300, height=300)]
        app = self._publish(windows)
        self.assertEqual([t.xid for t in app.cache.targets], [1, 2, 3])
        self.assertEqual(app.cache.desktop, 0)

    def test_a_crop_is_only_offered_where_nothing_covers_the_window(self):
        windows = [win(1, x=0, y=0, width=300, height=300),
                   win(2, x=50, y=50, width=300, height=300)]
        app = self._publish(windows)
        # 1 is behind 2, so a crop of the screen there would show window 2.
        self.assertEqual(sorted(app._window_rects), [2])

    def test_a_minimized_window_is_neither_read_nor_cropped(self):
        windows = [win(1, x=0, y=0, width=300, height=300),
                   win(2, x=0, y=0, width=300, height=300, minimized=True)]
        app = self._publish(windows)
        self.assertEqual([t.xid for t in app.cache.targets], [1])
        # Window 1 has nothing in front of it, so a crop is a fine *fallback*
        # for it; the minimized window is the one that must not be cropped,
        # since its rect is only where it used to sit.
        self.assertEqual(sorted(app._window_rects), [1])

    def test_an_unplaceable_window_is_not_published(self):
        windows = [win(1, x=0, y=0, width=300, height=300),
                   win(2, placed=False, x=900, y=0, width=300, height=300)]
        app = self._publish(windows)
        self.assertEqual([t.xid for t in app.cache.targets], [1])

    def test_only_this_workspace_is_published(self):
        windows = [win(1, desktop=0), win(2, desktop=1), win(3, desktop=-1)]
        app = self._publish(windows)
        self.assertEqual([t.xid for t in app.cache.targets], [1, 3])

    def test_the_frame_is_what_gets_read(self):
        windows = [win(1, x=0, y=0, width=300, height=300, frame=0xABC)]
        app = self._publish(windows)
        self.assertEqual(app.cache.targets[0].frame, 0xABC,
                         "the compositor redirects the frame, not the client")

    def test_a_window_without_a_frame_falls_back_to_its_own_id(self):
        windows = [win(7, x=0, y=0, width=300, height=300)]
        app = self._publish(windows)
        self.assertEqual(app.cache.targets[0].frame, 7)

    def test_the_workspace_is_stamped_on_the_targets(self):
        app = self._publish([win(1)], desktop=2)
        self.assertEqual(app.cache.desktop, 2)
        self.assertEqual(app._rects_desktop, 2)


class DesktopSwitchCaptureTests(unittest.TestCase):
    """A workspace switch invalidates the whole screen, and the first grab after
    it can be taken mid-animation -- so the app asks again, once, when the
    switch has definitely finished."""

    def _app(self, desktop=1, was=0):
        app = WinTab(Config(), use_overlay=False)
        app.wm = _FakeWindowManager([win(1)], desktop=desktop)
        app.cache = _FakeCache()
        app._desktop = was
        return app

    def test_the_switch_asks_twice_for_pixels(self):
        app = self._app()
        app._on_desktop_change()
        self.assertEqual(app.cache.urgencies, 1, "the grab taken right now")
        self.assertTrue(app._desktop_confirm, "and one for after the animation")
        self.addCleanup(self._drop, app)
        app._confirm_desktop_capture()
        self.assertEqual(app.cache.urgencies, 2)
        self.assertEqual(app._desktop_confirm, 0, "one shot, not a loop")

    def test_a_second_switch_replaces_the_pending_capture(self):
        app = self._app()
        app._on_desktop_change()
        first = app._desktop_confirm
        app.wm._desktop = 2
        app._on_desktop_change()
        self.assertNotEqual(app._desktop_confirm, first)
        self.addCleanup(self._drop, app)
        app._confirm_desktop_capture()
        self.assertEqual(app.cache.urgencies, 3)

    @staticmethod
    def _drop(app):
        if app._desktop_confirm:
            from gi.repository import GLib

            GLib.source_remove(app._desktop_confirm)
            app._desktop_confirm = 0


def _window_manager_error(force=False):
    raise XError(None, b"\x00" * 32)


class PublishSurvivesErrorsTests(unittest.TestCase):
    def test_a_broken_window_list_does_not_disturb_what_is_published(self):
        app = WinTab(Config(), use_overlay=False)
        manager = _FakeWindowManager([win(1)])
        manager.list_windows = _window_manager_error
        app.wm = manager
        app.cache = _FakeCache()
        app._refresh_rects()  # must not raise
        self.assertEqual(app.cache.targets, [])


class RootPositionTests(unittest.TestCase):
    """The bug that made every preview look like a random screenshot.

    ``get_geometry`` answers in the parent: 10 pixels inside the titlebar frame
    for a reparented window. Cropping a screen grab there showed the top-left of
    the desktop, not the window, and it did so silently for every card.
    """

    def test_the_position_is_asked_of_the_root(self):
        root, window = _Root(), _Window()
        self.assertEqual(root_position(root, window), (1031, 89))
        self.assertEqual(len(root.asked), 1, "the root must be the one asked")
        self.assertEqual(root.asked[0][0], window)
        self.assertEqual(root.asked[0][1:], (0, 0), "the window's own origin")
        self.assertEqual(window.translated_of, [], "never ask a window where it is")

    def test_an_unusable_reply_is_not_a_position(self):
        # A reply with the wrong field names must come back as "unknown" so the
        # caller shows a blank card, rather than as a plausible-looking offset.
        self.assertIsNone(root_position(_Root(reply=_LegacyReply()), _Window()))

    def test_a_root_that_cannot_answer_is_not_placed(self):
        self.assertIsNone(root_position(_BrokenRoot(), _Window()))

    def test_a_direct_child_of_the_root_is_its_own_frame(self):
        root = _Root()
        window = _Window(parent=SimpleNamespace(id=root.id))
        self.assertEqual(frame_of(root, window), window.id)

    def test_a_reparented_window_is_read_through_its_frame(self):
        root = _Root()
        window = _Window(parent=SimpleNamespace(id=0x2600608))
        self.assertEqual(frame_of(root, window), 0x2600608)

    def test_an_unreadable_frame_is_reported_as_none(self):
        root = _Root()
        window = _Window(parent="broken")
        self.assertEqual(frame_of(root, window), 0)

    def test_the_rect_uses_the_root_position_not_the_parent_one(self):
        # Taken from the machine this was built on: a Cinnamon Settings window
        # whose client sits at (10, 40) inside a frame at (1031, 89), with a
        # 32px titlebar above it. The crop has to start at the frame's corner,
        # 1031,57 -- not at 10,40, which is a slice of the desktop's top-left.
        manager = object.__new__(WindowManager)
        manager.root = _Root()
        manager.atom = lambda name: name
        window = _Window()
        window.get_geometry = lambda: SimpleNamespace(x=10, y=40, width=800, height=626)
        window.get_full_property = (
            lambda atom, kind: SimpleNamespace(value=[0, 0, 32, 0])
            if atom == "_NET_FRAME_EXTENTS" else None
        )
        self.assertEqual(manager._read_frame_geometry(window), (1031, 57, 800, 658))
        self.assertEqual(window.translated_of, [], "the window was never asked")
        self.assertTrue(manager._is_placed(window))

    def test_an_unplaceable_window_reports_itself_as_unplaced(self):
        manager = object.__new__(WindowManager)
        manager.root = _Root(reply=_LegacyReply())
        self.assertFalse(manager._is_placed(_Window()))


class _FakeDisplay:
    """Just enough display for an activation request to be built and sent."""

    def __init__(self):
        self.interned: list[str] = []
        self.flushed = False

    def intern_atom(self, name: str) -> int:
        self.interned.append(name)
        return 4242

    def create_resource_object(self, kind: str, xid: int):
        return xid  # the request builder accepts a plain window id

    def flush(self) -> None:
        self.flushed = True


class _FakeRoot:
    def __init__(self):
        self.sent: list = []

    def send_event(self, event, event_mask=None) -> None:
        self.sent.append((event, event_mask))


class ActivateTests(unittest.TestCase):
    """Switching silently stopped working once: the request quoted a stale
    timestamp, the window manager refused it for focus-stealing reasons, and
    nothing changed -- so the timestamp was never refreshed and every later
    swipe was refused too. It was invisible, because the log said "switched"."""

    def _manager(self, last_event_time: int):
        manager = object.__new__(WindowManager)
        manager.display = _FakeDisplay()
        manager.root = _FakeRoot()
        manager._atoms = {}
        manager._last_event_time = last_event_time
        return manager

    def test_the_request_quotes_the_current_time(self):
        manager = self._manager(last_event_time=11667307)
        manager.activate(0x3600004)
        event, mask = manager.root.sent[0]
        self.assertEqual(event.window, 0x3600004)
        self.assertEqual(event.data[1][0], 2, "source indication: a pager")
        self.assertEqual(event.data[1][1], X.CurrentTime)
        self.assertNotEqual(event.data[1][1], 11667307, "never the stale time")
        self.assertTrue(manager.display.flushed)
        self.assertIn("_NET_ACTIVE_WINDOW", manager.display.interned)

    def test_it_asks_the_window_manager_to_promote_the_window(self):
        manager = self._manager(0)
        manager.activate(0x5E00007)
        _, mask = manager.root.sent[0]
        self.assertEqual(mask, X.SubstructureRedirectMask | X.SubstructureNotifyMask)


class _EventDisplay:
    """A display that hands out a fixed list of events, then nothing."""

    def __init__(self, events):
        self.events = list(events)

    def pending_events(self) -> bool:
        return bool(self.events)

    def next_event(self):
        return self.events.pop(0)

    def intern_atom(self, name: str) -> int:
        return {
            "_NET_ACTIVE_WINDOW": 100,
            "_NET_CLIENT_LIST": 101,
            "_NET_CURRENT_DESKTOP": 102,
        }.get(name, 999)

    def get_atom_name(self, atom: int) -> str:
        return {
            100: "_NET_ACTIVE_WINDOW",
            101: "_NET_CLIENT_LIST",
            102: "_NET_CURRENT_DESKTOP",
        }.get(atom, "_NET_WM_NAME")

    def fileno(self) -> int:
        return -1  # select() on this raises, which the reader treats as "done"


class _EventRoot:
    def __init__(self, active: int = 0):
        self.active = active

    def get_full_property(self, atom, kind):
        if atom == 100 and self.active:
            return SimpleNamespace(value=[self.active])
        return None


class DesktopHookTests(unittest.TestCase):
    """A workspace switch has to be noticed even when focus does not move,
    because every pixel the cache holds belongs to the workspace we left."""

    def _manager(self, events, active: int = 0):
        manager = object.__new__(WindowManager)
        manager.display = _EventDisplay(events)
        manager.root = _EventRoot(active)
        manager._atoms = {}
        manager._atom_text = {}
        manager._errors = []
        manager._last_event_time = 0
        manager._watching = True
        manager.focus_calls = 0
        manager.desktop_calls = 0
        manager.mru = MruTracker()
        manager.on_focus_change = lambda: setattr(
            manager, "focus_calls", manager.focus_calls + 1
        )
        manager.on_desktop_change = lambda: setattr(
            manager, "desktop_calls", manager.desktop_calls + 1
        )
        return manager

    @staticmethod
    def _notify(atom: int, when: int = 7):
        return SimpleNamespace(type=X.PropertyNotify, atom=atom, time=when)

    def test_a_workspace_switch_calls_the_desktop_hook(self):
        manager = self._manager([self._notify(102)])
        manager.drain_events()
        self.assertEqual(manager.desktop_calls, 1)
        self.assertEqual(manager.focus_calls, 0)

    def test_a_focus_change_still_calls_the_focus_hook(self):
        manager = self._manager([self._notify(100)], active=0x5C00009)
        manager.drain_events()
        self.assertEqual(manager.focus_calls, 1)
        self.assertEqual(manager.desktop_calls, 0)
        self.assertEqual(manager.mru.order, [0x5C00009])

    def test_unrelated_property_changes_are_ignored(self):
        manager = self._manager([self._notify(999)])
        manager.drain_events()
        self.assertEqual((manager.focus_calls, manager.desktop_calls), (0, 0))

    def test_an_empty_event_queue_is_not_an_error(self):
        manager = self._manager([])
        manager.drain_events()
        self.assertEqual((manager.focus_calls, manager.desktop_calls), (0, 0))


class MruTrackerTests(unittest.TestCase):
    def test_focus_moves_a_window_to_the_front(self):
        tracker = MruTracker([1, 2, 3])
        self.assertEqual(tracker.order, [1, 2, 3])
        tracker.note(3)
        self.assertEqual(tracker.order, [3, 1, 2])

    def test_ranking(self):
        tracker = MruTracker([1, 2])
        self.assertEqual(tracker.rank(1), 0)
        self.assertEqual(tracker.rank(2), 1)
        self.assertGreater(tracker.rank(99), 100)

    def test_forget(self):
        tracker = MruTracker([1, 2])
        tracker.forget(1)
        self.assertEqual(tracker.order, [2])

    def test_seeding_from_stacking_puts_the_top_window_first(self):
        tracker = MruTracker()
        tracker.seed_from_stacking([1, 2, 3])  # bottom-to-top
        self.assertEqual(tracker.order, [3, 2, 1])

    def test_note_ignores_bogus_ids(self):
        tracker = MruTracker()
        tracker.note(0)
        self.assertEqual(tracker.order, [])

    def test_order_is_bounded(self):
        tracker = MruTracker()
        for xid in range(1, 500):
            tracker.note(xid)
        self.assertLessEqual(len(tracker.order), 256)
        self.assertEqual(tracker.order[0], 499)


class _StickyWindow:
    def __init__(self):
        self.properties: dict = {}

    def change_property(self, prop, type_, format_, data):
        self.properties[prop] = (type_, format_, list(data))


class _StickyDisplay:
    """Just enough of a python-xlib Display for the sticky request."""

    def __init__(self):
        self.atoms: dict[str, int] = {}
        self.windows: dict[int, _StickyWindow] = {}
        self.flushed = 0

    def intern_atom(self, name):
        return self.atoms.setdefault(name, len(self.atoms) + 100)

    def create_resource_object(self, kind, ident):
        return self.windows.setdefault(int(ident), _StickyWindow())

    def flush(self):
        self.flushed += 1


class _StickyRoot:
    def __init__(self):
        self.sent: list = []

    def send_event(self, message, event_mask=None):
        self.sent.append((message, event_mask))


class MakeStickyTests(unittest.TestCase):
    """An overlay the window manager keeps on one workspace is invisible on
    every other one, however hard the gesture works."""

    def test_the_all_desktops_property_is_written(self):
        display, root = _StickyDisplay(), _StickyRoot()
        self.assertTrue(make_sticky(display, root, 0x5A00003))
        prop = display.windows[0x5A00003].properties[display.atoms["_NET_WM_DESKTOP"]]
        self.assertEqual(prop[0], Xatom.CARDINAL)
        self.assertEqual(prop[2], [0xFFFFFFFF])

    def test_the_sticky_state_message_is_sent_to_the_root_window(self):
        display, root = _StickyDisplay(), _StickyRoot()
        make_sticky(display, root, 0x5A00003)
        message, mask = root.sent[0]
        self.assertEqual(message.client_type, display.atoms["_NET_WM_STATE"])
        self.assertEqual(message.data[1][0], 1, "_NET_WM_STATE_ADD")
        self.assertEqual(message.data[1][1], display.atoms["_NET_WM_STATE_STICKY"])
        self.assertTrue(mask & X.SubstructureRedirectMask)
        self.assertEqual(display.flushed, 1)

    def test_a_broken_connection_is_reported_not_raised(self):
        class Dead:
            def intern_atom(self, name):
                raise XError(None, b"\x00" * 32)

        self.assertFalse(make_sticky(Dead(), _StickyRoot(), 1))


class SwitchResortsTests(unittest.TestCase):
    """After every switch the new window is the first card, so the next swipe
    starts from where you are (that is what "recent" order buys)."""

    def _app(self, windows, mru=()):
        app = WinTab(Config(), use_overlay=False, log=lambda *_: None)
        wm = _FakeWindowManager(windows)
        wm.mru = MruTracker(list(mru))
        app.wm = wm
        app.cache = _FakeCache()
        app._entries = build_app_list(windows, [w.xid for w in windows], 0, group_by_app=False)
        return app, wm

    def test_committing_notes_the_window_so_it_is_card_zero_next_time(self):
        windows = [win(1), win(2), win(3)]
        app, wm = self._app(windows, mru=[1, 2, 3])
        app._commit(2)  # the card for window 3
        self.assertEqual(wm.activated, [3])
        self.assertEqual(wm.mru.order[0], 3)
        # Which is the same thing as "the row the next strip builds starts
        # with it" -- the app lines the cards up from this one order.
        apps = build_app_list(
            windows, wm.mru.order + [w.xid for w in windows], 0, group_by_app=False
        )
        self.assertEqual([a.windows[0].xid for a in apps], [3, 1, 2])
        self.assertEqual(entry_index(apps, 3), 0)

    def test_a_finished_gesture_refreshes_the_order_from_the_stack(self):
        app, wm = self._app([win(1), win(2)])
        app._after_gesture()
        self.assertEqual(wm.refreshed, 1)
        self.assertEqual(app._entries, [])


if __name__ == "__main__":
    unittest.main()
