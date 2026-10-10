"""Wiring: touchpad thread -> gesture state machine -> carousel -> window switch.

The threading model is deliberately small:

* the **touchpad thread** reads raw evdev frames and turns them into gesture
  events. It never touches X11, GTK or PIL.
* the **capture thread** (in ``capture.py``) keeps the screen snapshot current
  and hands each fresh frame to the carousel's ``prewarm``. It touches PNG/PIL
  and cairo, but no X11 objects of ours.
* the **GTK main thread** owns everything else: the EWMH reads, the overlay
  window, the animation, and the decision to switch windows.

The only channel between them is a deque of gesture events, handed over with
``GLib.idle_add`` (coalesced, so a burst of frames costs one main-loop callout).
"""

from __future__ import annotations

import os
import signal
import threading
import time

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

from . import config as config_module  # noqa: E402
from .apps import WindowManager, usable_windows, windows_needing_own_pixels  # noqa: E402
from .capture import ScreenCache, WindowTarget  # noqa: E402
from .gesture import (  # noqa: E402
    GestureParams,
    SwipeBegin,
    SwipeCancel,
    SwipeEnd,
    SwipeTap,
    SwipeUpdate,
    ThreeFingerSwipe,
    clamp_steps,
)
from .overlay import Carousel  # noqa: E402
from .pointer import PointerLock  # noqa: E402
from .touchpad import TouchpadReader, format_frames, iter_replay_frames  # noqa: E402

TOUCHPAD_RETRY_S = 5.0  # how often the window list is re-read while idle
WATCHDOG_MS = 250  # how often the safety net checks for a stuck gesture
QUIET_S = 0.6  # no frames for this long mid-swipe means the gesture is over
CONFIRM_MS = 250  # how long to give the window manager before checking a switch
DESKTOP_SETTLE_MS = 700  # a second grab after a workspace switch, once it has settled


def _process_running(name: str) -> bool:
    """Is a process whose command line contains `name` running?"""
    try:
        entries = os.listdir("/proc")
    except OSError:
        return False
    for entry in entries:
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as handle:
                command = handle.read().decode("utf-8", "replace")
        except OSError:
            continue
        if name in command:
            return True
    return False


def cinnamon_gestures_enabled() -> bool | None:
    """True/False, or None when Cinnamon's gesture schema is not present."""
    try:
        try:
            gi.require_version("Gio", "2.0")
        except ValueError:
            pass
        from gi.repository import Gio

        source = Gio.SettingsSchemaSource.get_default()
        if source is None or source.lookup("org.cinnamon.gestures", True) is None:
            return None
        return bool(Gio.Settings.new("org.cinnamon.gestures").get_boolean("enabled"))
    except Exception:
        return None


def find_conflicts() -> list[str]:
    """Other gesture handlers that would fight us for the same three fingers.

    Returns a list of ready-to-print warnings, each with the concrete fix, so
    the user is never left wondering why a swipe tiles a window instead.
    """
    warnings: list[str] = []

    if _process_running("touchegg"):
        warnings.append(
            "Touchegg is running and already maps three-finger left/right to\n"
            "    TILE_WINDOW. Disable it (needs sudo):\n"
            "        sudo systemctl disable --now touchegg\n"
            "    or keep it for other gestures by writing a user config at\n"
            "        ~/.config/touchegg/touchegg.conf\n"
            "    that leaves out the three-finger left/right actions."
        )

    enabled = cinnamon_gestures_enabled()
    if enabled:
        warnings.append(
            "Cinnamon's own gestures are enabled and map three-finger\n"
            "    left/right to workspace switching. Turn them off:\n"
            "        dconf write /org/cinnamon/gestures/enabled false\n"
            "    Prefer gsettings? Use /usr/bin/gsettings explicitly: a gsettings\n"
            "    from Homebrew writes to a keyfile that Cinnamon never reads."
        )

    return warnings


class WinTab:
    """The application object: everything lives here so nothing is global."""

    def __init__(self, config, use_overlay: bool = True, verbose: bool | None = None,
                 record_path: str | None = None, log=None):
        self.config = config
        self.use_overlay = use_overlay
        self.verbose = config.verbose if verbose is None else verbose
        self.log = log or print
        self.record_path = record_path

        self.wm = WindowManager(config)
        self.wm.on_focus_change = self._on_focus_change
        self.wm.on_desktop_change = self._on_desktop_change
        self.swipe = ThreeFingerSwipe(GestureParams.from_config(config))
        self.cache = ScreenCache(config, on_update=self._on_capture) if use_overlay else None
        # The overlay is created on the workspace winTab starts in, and the
        # window manager will only ever composite it there unless it is told
        # otherwise before the window is first mapped.
        self.overlay = (
            Carousel(
                config,
                make_sticky=self.wm.make_sticky,
                clear_attention=self.wm.clear_attention,
            )
            if use_overlay
            else None
        )
        self.lock = PointerLock()

        self._queue: list = []
        self._queue_lock = threading.Lock()
        self._scheduled = False
        self._loop: GLib.MainLoop | None = None
        self._reader: TouchpadReader | None = None
        self._replay_thread: threading.Thread | None = None
        self._record_handle = None

        self._entries: list = []
        self._index = 0
        self._window_rects: dict[int, tuple[int, int, int, int]] = {}
        self._desktop = -1  # the workspace we last noticed
        self._rects_desktop = -1  # the workspace the published rects belong to
        self._rects_refreshed_at = 0.0  # when the window list was last re-read
        self._setup_done = False
        self._desktop_confirm = 0  # pending post-switch capture id
        self._grabbed = False
        self._last_frame_at = 0.0
        self._logged_progress = 0.0

    # -- setup -------------------------------------------------------------
    def setup(self) -> None:
        if self._setup_done:
            # Setting up twice would start a second capture thread sharing one X
            # display with the first, which python-xlib cannot survive.
            return
        self._setup_done = True
        Gtk.init([])
        if self.overlay is not None:
            self.overlay.ensure_window()
            self._refresh_rects()
            self.cache.start()
        self.wm.watch_focus()
        GLib.io_add_watch(self.wm.fileno(), GLib.IO_IN, self._on_x_events)
        GLib.timeout_add(int(TOUCHPAD_RETRY_S * 1000), self._periodic)
        GLib.timeout_add(WATCHDOG_MS, self._watchdog)

    def start(self) -> None:
        if self.record_path:
            try:
                self._record_handle = open(self.record_path, "a", encoding="utf-8")
            except OSError as exc:
                self.log(f"wintab: cannot record to {self.record_path}: {exc}")
        self._reader = TouchpadReader(
            self._on_frames,
            name=self.config.touchpad_name,
            on_device_change=self._on_device,
        )
        self._reader.start()

    def replay(self, path: str) -> None:
        """Feed recorded frames through the same pipeline, in real time."""

        def worker():
            try:
                with open(path, "r", encoding="utf-8") as handle:
                    for frame in iter_replay_frames(handle):
                        self._on_frames([frame])
            except OSError as exc:
                GLib.idle_add(self.log, f"wintab: cannot replay {path}: {exc}")
            GLib.idle_add(self._quit)

        self._replay_thread = threading.Thread(target=worker, name="wintab-replay", daemon=True)
        self._replay_thread.start()

    def run(self) -> int:
        self.setup()
        if self._replay_thread is None:
            self.start()
        self._loop = GLib.MainLoop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, self._quit)
        self.log("wintab: three-finger swipe left/right to switch apps (ctrl-c to stop)")
        try:
            self._loop.run()
        finally:
            self.shutdown()
        return 0

    def _quit(self, *_args) -> bool:
        if self._loop is not None:
            self._loop.quit()
        return False

    def shutdown(self) -> None:
        self._release_pointer()
        if self._reader is not None:
            self._reader.stop()
        if self.cache is not None:
            self.cache.stop()
        self.swipe.flush()
        if self.overlay is not None:
            self.overlay.abort()
            self.overlay.close()
        if self._record_handle is not None:
            try:
                self._record_handle.close()
            except OSError:
                pass
            self._record_handle = None

    # -- main-loop plumbing ------------------------------------------------
    def _on_x_events(self, _fd, _condition) -> bool:
        self.wm.drain_events()
        return True

    def _periodic(self) -> bool:
        """Keep the capture thread's targets current -- but only if anything moved.

        Re-reading the window list is a round trip per property per window on
        the GTK thread, and a stall like that landing on the first frames of a
        swipe is exactly what a swipe feels like when it stutters. The capture
        thread already knows when the screen last changed; if it has not since
        we last looked, no window has moved and there is nothing to re-read.
        """
        if not self._entries:
            if self.cache is None or self.cache.last_damage_at > self._rects_refreshed_at:
                self._refresh_rects()
        return True

    def _watchdog(self) -> bool:
        """Two safety nets: finish a gesture the pad stopped talking about, and
        never hold the pointer grab when nothing is on screen.

        A liftoff is normally reported, but a pad that drops the report, a
        device that disappears, or a suspended laptop would otherwise leave the
        carousel up for good. Treating silence as a release commits whatever
        the fingers had already dragged to, which is what letting go does.
        """
        now = time.monotonic()
        if self.swipe.active and self._last_frame_at and now - self._last_frame_at > QUIET_S:
            self.log(f"wintab: no frames for {now - self._last_frame_at:.1f}s mid-swipe; "
                     "treating it as a release")
            self._dispatch(self.swipe.flush())

        busy = self.swipe.active or (self.overlay is not None and self.overlay.visible)
        if self._grabbed and not busy:
            self._release_pointer()
        return True

    def _refresh_rects(self) -> None:
        """Publish this workspace's windows for the capture thread.

        Only this workspace's windows are worth snapshotting -- the strip never
        shows any others -- and only windows we could actually place on screen,
        because cropping anywhere else would put a random slice of the desktop
        on their card.

        Every one of them is asked for **its own pixels**, not just the ones
        something is in front of. A window's own pixels are the window whatever
        workspace is on screen and whatever the compositor is in the middle of;
        a crop of the screen is right only when the grab was taken on this
        workspace, after its switch animation finished, with nothing over the
        window -- three things that are all wrong at once exactly when you have
        just changed workspace. The reads cost ~25ms a window, on the capture
        thread, off the gesture path.
        """
        self._rects_refreshed_at = time.monotonic()
        try:
            # Forced: noticing that a window moved is the point of this pass.
            everything = self.wm.list_windows(force=True)  # bottom to top
            desktop = self.wm.current_desktop()
            windows = usable_windows(everything, desktop, self.wm._own_pid)
            covered = windows_needing_own_pixels(
                everything, desktop, self.wm.screen_geometry(), self.wm._own_pid
            )
        except Exception:
            return
        self._rects_desktop = desktop
        # A minimized window has no on-screen pixels anywhere: its card should be
        # an empty frame with its name, not a crop of whatever sits where it
        # used to be, and not its own stale pixels either.
        placed = [win for win in windows if win.placed and not win.minimized]
        # Crops are only the fallback for a window whose own pixels cannot be
        # read (no compositor, say). Publish a rect for the windows a crop would
        # be *right* for: the ones nothing is in front of.
        self._window_rects = {win.xid: win.rect for win in placed if win.xid not in covered}
        if self.cache is not None:
            self.cache.set_targets(
                [
                    WindowTarget(xid=win.xid, frame=win.frame or win.xid, rect=win.rect)
                    for win in placed
                ],
                desktop=desktop,
            )

    def _on_focus_change(self) -> None:
        self._note_desktop()
        self._refresh_rects()
        if self.cache is not None:
            self.cache.refresh_soon()

    def _on_desktop_change(self) -> None:
        """The workspace changed: everything we are holding is from the old one."""
        if self._note_desktop() and self.overlay is not None:
            self.overlay.forget_screen_cards()
        self._refresh_rects()
        if self.cache is not None:
            # Not even the settle wait: the strip must not be showing the
            # previous workspace when you swipe now.
            self.cache.refresh_soon(urgent=True)
            # And once more when the switch animation is over: the grab above is
            # taken as soon as the workspace changes, which can be mid-animation,
            # and a desktop that then holds still would never replace it.
            self._schedule_desktop_confirm()

    def _schedule_desktop_confirm(self) -> None:
        """One extra grab shortly after a workspace switch (replacing any pending one)."""
        if self._desktop_confirm:
            GLib.source_remove(self._desktop_confirm)
        self._desktop_confirm = GLib.timeout_add(DESKTOP_SETTLE_MS, self._confirm_desktop_capture)

    def _confirm_desktop_capture(self) -> bool:
        """Take the settled grab that the mid-switch one was not."""
        self._desktop_confirm = 0
        if self.cache is not None:
            self.cache.refresh_soon(urgent=True)
        return False  # one shot

    def _note_desktop(self) -> bool:
        """Remember which workspace we are on; True when it changed."""
        try:
            desktop = self.wm.current_desktop()
        except Exception:
            return False
        if desktop == self._desktop:
            return False
        self._desktop = desktop
        return True

    def _on_device(self, message: str, geometry) -> None:
        detail = ""
        if geometry is not None:
            detail = f" [{geometry.x_min}..{geometry.x_max} x {geometry.y_min}..{geometry.y_max}]"
        GLib.idle_add(self.log, f"wintab: touchpad {message}{detail}")

    # -- capture thread ----------------------------------------------------
    def _on_capture(self, generation: int) -> None:
        """Runs on the capture worker thread: build the carousel's pixels.

        Only reads plain data (the snapshot images and the rects the main thread
        last published), so no X11 or GTK call happens here.
        """
        if self.overlay is None or self.cache is None:
            return
        if self.cache.busy:
            # A swipe is on screen. Baking cards now would put ~200ms of PIL in
            # front of the frames the strip is drawing; the cards from the last
            # capture stay on screen instead, and _after_gesture asks for a
            # fresh capture the moment the fingers are off the pad.
            return
        snapshot = self.cache.snapshot()
        if snapshot is None:
            return
        plain, backdrop, window_pixels, desktop = snapshot
        try:
            self.overlay.prewarm(
                plain,
                backdrop,
                generation,
                self._window_rects,
                window_pixels,
                # The rects belong to the workspace we had when we asked, and the
                # grab says which workspace it is a picture of (read from the
                # server, not assumed): a crop is only allowed when they agree.
                allow_crop=desktop == self._rects_desktop >= 0,
                # Checked between windows: a swipe that starts while this is
                # running stops the rest of the work instead of waiting for it.
                cancel=lambda: self.cache is not None and self.cache.busy,
            )
        except Exception as exc:
            GLib.idle_add(self.log, f"wintab: could not prepare overlay pixels: {exc}")

    # -- input thread ------------------------------------------------------
    def _on_frames(self, frames) -> None:
        self._last_frame_at = time.monotonic()
        if self._record_handle is not None:
            try:
                self._record_handle.write(format_frames(frames) + "\n")
                self._record_handle.flush()
            except OSError:
                pass

        events: list = []
        for frame in frames:
            events.extend(self.swipe.feed(frame))
        if not events:
            return

        with self._queue_lock:
            self._queue.extend(events)
            if self._scheduled:
                return
            self._scheduled = True
        GLib.idle_add(self._drain)

    def _drain(self) -> bool:
        with self._queue_lock:
            self._scheduled = False
            events, self._queue = self._queue, []
        self._dispatch(events)
        return False

    def _dispatch(self, events) -> None:
        for event in events:
            try:
                self._handle(event)
            except Exception as exc:  # one bad frame must not kill the app
                self.log(f"wintab: gesture handling failed: {exc!r}")

    # -- gestures (main thread) --------------------------------------------
    def _handle(self, event) -> None:
        if isinstance(event, SwipeBegin):
            self._begin()
        elif isinstance(event, SwipeUpdate):
            if self.overlay is not None:
                # The engine's own selection goes along, so the outlined card and
                # the card that opens come from one rule and cannot drift apart.
                self.overlay.set_progress(event.progress, event.steps)
            # A still three-finger hold repeats the same value dozens of times a
            # second; only log movement, or the log buries its own decisions.
            if self.verbose and abs(event.progress - self._logged_progress) >= 0.01:
                self.log(f"wintab:   dragging p={event.progress:+.3f}")
                self._logged_progress = event.progress
        elif isinstance(event, SwipeEnd):
            self._end(event)
        elif isinstance(event, SwipeCancel):
            self.log(f"wintab: cancelled ({event.reason})")
            self._cancel()
        elif isinstance(event, SwipeTap):
            if self.verbose:
                self.log("wintab: three-finger tap (nothing bound)")

    def _begin(self) -> None:
        self._logged_progress = 0.0
        self._refresh_rects()
        self._entries, index = self.wm.build()
        self._index = index
        if len(self._entries) <= 1:
            self.log("wintab: only one window in this workspace, nothing to switch to")
            self._entries = []
            return

        if self.verbose and self.cache is not None:
            self.log(f"wintab: cache {self.cache.stats}")
        self.log(
            f"wintab: strip open -- {len(self._entries)} window(s) here, "
            f"on {self._label(index)}"
            + (f", others: {' | '.join(self._label(j) for j in range(len(self._entries)) if j != index)}"
               if self.verbose else "")
        )
        if self.overlay is not None:
            self.cache.set_busy(True)
            self.overlay.open(self._entries, index)
            if self.overlay.count != len(self._entries):
                # A card could not be built for some window, which would shift
                # every index: better to do nothing than to switch to the
                # wrong window.
                self.log("wintab: could not build a card for every window, skipping this swipe")
                self.overlay.abort()
                self._entries = []
                self.cache.set_busy(False)
                return
        self._grab_pointer()

    def _end(self, event: SwipeEnd) -> None:
        if not self._entries:
            self._release_pointer()
            return
        steps = clamp_steps(event.steps, self._index, len(self._entries))
        velocity = f"{event.velocity:+.2f} steps/s"
        if steps == 0:
            self.log(
                f"wintab: release {event.progress:+.2f} ({velocity}) -> "
                f"stay on {self._label(self._index)}"
            )
            self._cancel()
            return

        target = self._index + steps
        self.log(
            f"wintab: release {event.progress:+.2f} ({velocity}) -> "
            f"{steps:+d} app(s) -> {self._label(target)}"
        )
        if self.overlay is None:
            self._commit(target)
            self._after_gesture()
            return
        self.overlay.settle(
            steps,
            on_commit=lambda: self._commit(target),
            on_done=self._after_gesture,
        )

    def _cancel(self) -> None:
        self._release_pointer()
        if self.overlay is None or not self.overlay.visible:
            self._after_gesture()
            return
        self.overlay.cancel(on_done=self._after_gesture)

    def _commit(self, target: int) -> None:
        if not (0 <= target < len(self._entries)):
            return
        entry = self._entries[target]
        top = entry.top
        self.wm.activate(top.xid)
        self.wm.mru.note(top.xid)
        self._index = target
        self.log(f"wintab:   switched to {entry.label} (0x{top.xid:x})")
        # A refused switch used to be invisible: the log said "switched" while
        # the window stayed where it was. Check, and say so.
        GLib.timeout_add(CONFIRM_MS, self._confirm_switch, top.xid, entry.label)

    def _confirm_switch(self, xid: int, label: str) -> bool:
        """Did the window actually come to the front? One retry, then admit it."""
        active = self.wm.active_window()
        if active == xid:
            return False
        self.log(
            f"wintab: {label} did not take focus (active is 0x{active or 0:x}); asking again"
        )
        self.wm.activate(xid)
        return False

    def _after_gesture(self) -> None:
        self._release_pointer()
        self._entries = []
        if self.cache is not None:
            # Captures were paused for the overlay; freshen them and resume.
            self.cache.set_busy(False)
            self.cache.refresh_soon()
        self.wm.refresh_mru()

    def _label(self, index: int) -> str:
        """A human name for a card.

        With one card per window, four browser windows all answer to the same
        app name, so the window's title is what tells them apart in the log.
        """
        if not (0 <= index < len(self._entries)):
            return "?"
        entry = self._entries[index]
        if len(entry.windows) > 1:
            return f"{entry.label} +{len(entry.windows) - 1}"
        title = (entry.top.title or "").strip()
        return title[:52] if title else entry.label

    # -- pointer -----------------------------------------------------------
    def _grab_pointer(self) -> None:
        if not self.config.freeze_pointer or self._grabbed:
            return
        if self.lock.grab():
            self._grabbed = True
        else:
            self.log(f"wintab: pointer freeze unavailable ({self.lock.error})")

    def _release_pointer(self) -> None:
        if not self._grabbed:
            return
        self._grabbed = False
        self.lock.release()


def run(config=None, **kwargs) -> int:
    """Convenience entry point used by the CLI."""
    app = WinTab(config or config_module.load(), **kwargs)
    warnings = find_conflicts()
    for warning in warnings:
        print(f"wintab: WARNING: {warning}")
    return app.run()
