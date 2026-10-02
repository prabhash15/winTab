"""Screen snapshots and the dimmed, blurred backdrop behind the carousel.

The carousel needs two things the moment a swipe starts: the current screen as
pixels (to crop each app's window out of) and a Windows 11-ish backdrop (the
same screen, blurred and dimmed). Both are far too slow to produce on demand --
measured at ~60ms for the grab and ~60ms for the blur on this machine -- so they
are built ahead of time by a worker thread and frozen when a gesture begins.

Freshness comes from the DAMAGE extension, used in an arm/disarm style: after
every capture we arm a damage object on the root window, and the first change
disarms it. That means zero wakeups while the desktop is idle and exactly one
wakeup per settled change -- and it still captures every couple of seconds while
something is animating (a video), because the settle timer is bounded.
"""

from __future__ import annotations

import dataclasses
import os
import select
import threading
import time
from collections.abc import Sequence

from PIL import Image, ImageEnhance, ImageFilter
from Xlib import X, display as xdisplay
from Xlib.error import XError

try:  # pragma: no cover - depends on the X server
    from Xlib.ext import damage as _damage

    _HAVE_DAMAGE = True
except ImportError:  # pragma: no cover
    _damage = None
    _HAVE_DAMAGE = False

try:  # pragma: no cover - depends on the X server
    from Xlib.ext import composite as _composite

    _HAVE_COMPOSITE = True
except ImportError:  # pragma: no cover
    _composite = None
    _HAVE_COMPOSITE = False

# Longest we will wait for a continuously changing screen to hold still.
#: Longest the worker waits for the screen to hold still before grabbing anyway.
#: This bounds how stale a card can be after a burst of changes (a window
#: opening, a workspace switch): a longer wait buys a calmer picture at the cost
#: of showing an older one, and it also caps how long the *backdrop* sits in the
#: queue behind that wait while a swipe starts.
MAX_SETTLE_WAIT = 1.0
# ...but when the whole desktop has just changed (a workspace switch), the wait
# costs a swipe's worth of stale cards, so we take the first quiet moment.
URGENT_SETTLE_WAIT = 0.25


def image_from_ximage(data: bytes, width: int, height: int) -> Image.Image | None:
    """Decode raw ``ZPixmap`` bytes (depth 24 or 32) into an RGB image.

    Both depths are laid out as B, G, R, pad in memory, so one decoder covers
    a root grab and a window's own Composite pixmap alike.
    """
    if width <= 0 or height <= 0 or len(data) < width * height * 4:
        return None
    try:
        return Image.frombytes("RGB", (width, height), data[: width * height * 4], "raw", "BGRX")
    except (ValueError, OSError):
        return None


def grab_screen(display, root) -> Image.Image | None:
    """The whole root window as an image, or None if it cannot be read."""
    try:
        geometry = root.get_geometry()
        raw = root.get_image(0, 0, geometry.width, geometry.height, X.ZPixmap, 0xFFFFFFFF).data
    except (XError, ValueError, OSError):
        return None
    return image_from_ximage(raw, geometry.width, geometry.height)


def read_current_desktop(display, root) -> int:
    """Which workspace the server says is on screen, or -1 if it will not say.

    Read on the capture thread, from its own connection, right before a grab:
    the workspace a grab belongs to is a *fact about the grab*, not about what
    the app happened to ask for. Stamping the request instead is how a picture
    of the workspace you just left gets filed as the one you are on look like a
    valid source for every card after it.
    """
    try:
        atom = display.intern_atom("_NET_CURRENT_DESKTOP")
        prop = root.get_full_property(atom, X.AnyPropertyType)
        if prop is None or not len(prop.value):
            return -1
        return int(prop.value[0])
    except (XError, AttributeError, TypeError, ValueError, OSError):
        return -1


def stamped_desktop(observed: int, requested: int) -> int:
    """The desktop a grab belongs to: what the server said, else what we asked.

    ``-1`` (unreadable) keeps the requested desktop so the app still has a
    stamp to compare against; ``read_current_desktop`` returning -1 while the
    app asked for a real workspace can only happen if the property vanished, in
    which case nothing can be said about the grab's workspace either.
    """
    return observed if observed >= 0 else requested


def mean_difference(a: Image.Image, b: Image.Image) -> float:
    """Mean absolute difference per colour channel, 0 (identical) to 255."""
    if a.size != b.size:
        b = b.resize(a.size)
    left = a.convert("RGB").tobytes()
    right = b.convert("RGB").tobytes()
    if not left:
        return 0.0
    return sum(abs(x - y) for x, y in zip(left, right)) / len(left)


@dataclasses.dataclass(frozen=True)
class WindowTarget:
    """One window worth snapshotting.

    ``frame`` is the window the compositor actually redirects: our window
    manager reparents clients into a titlebar frame, and a card built from the
    frame holds the decorated window -- titlebar and all -- even when something
    else is in front of it. ``rect`` is the fallback: the window's place on
    screen, to crop out of the full-screen grab if the frame cannot be read.
    """

    xid: int
    frame: int
    rect: tuple[int, int, int, int]

    @property
    def size(self) -> tuple[int, int]:
        return (self.rect[2], self.rect[3])


def read_window_pixels(display, frame: int) -> Image.Image | None:
    """A window's own pixels, straight from the compositor.

    Cropping the screen at a window's rect is cheaper but wrong the moment
    anything overlaps it: the card then shows whatever was in front. Naming the
    window's off-screen pixmap gives the window itself, decorated, whatever is
    on top of it. Only redirected windows have one -- the root's children, not
    the clients nested inside titlebar frames -- hence passing the frame.

    Measured at 27ms for a 1440x900 window on the machine this was built for,
    on the capture thread, and only once the screen has settled.
    """
    if not _HAVE_COMPOSITE or not frame:
        return None
    try:
        handle = display.create_resource_object("window", frame)
        geometry = handle.get_geometry()
        if geometry is None or geometry.width <= 1 or geometry.height <= 1:
            return None
        pixmap = handle.composite_name_window_pixmap()
        try:
            raw = pixmap.get_image(
                0, 0, geometry.width, geometry.height, X.ZPixmap, 0xFFFFFFFF
            )
            data = bytes(raw.data) if raw is not None else b""
        finally:
            pixmap.free()
        display.flush()
    except (XError, AttributeError, ValueError, OSError):
        # Not redirected, resized under us, or gone: the caller falls back to
        # cropping the screen, which is still right when nothing covers it.
        return None
    return image_from_ximage(data, geometry.width, geometry.height)


class ScreenCache:
    """Keeps `plain` and `backdrop` up to date in a background thread."""

    def __init__(self, config, on_update=None):
        self.config = config
        self._on_update = on_update
        self._lock = threading.Lock()
        self._plain: Image.Image | None = None
        self._backdrop: Image.Image | None = None
        self._windows: dict[int, Image.Image] = {}
        self._targets: Sequence[WindowTarget] = ()
        self._desktop = -1
        self._urgent = threading.Event()
        self._generation = 0
        self._requests = 0
        self._captured_at = 0.0
        self._capture_ms = 0.0
        self._last_error: str | None = None
        self._busy = threading.Event()
        self._dirty = threading.Event()
        self._stop = threading.Event()
        self._captured_desktop = -1
        # A pipe to poke the worker out of its select(): "refresh_soon" used to
        # wait for the next poll, up to half a second, which is a workspace
        # switch's worth of cards showing the wrong desktop.
        self._wake_r, self._wake_w = os.pipe()
        self._thread: threading.Thread | None = None
        self._display = None
        self._root = None
        self._damage_id = None
        self._last_damage_at = 0.0
        self._geometry = (0, 0, 0, 0)

    # -- public API --------------------------------------------------------
    def start(self) -> None:
        if self._thread is not None and self._thread.is_alive():
            # Starting twice would give two threads one X display, and python-xlib
            # replies get crossed the moment that happens.
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="wintab-capture", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self.refresh_soon()
        if self._thread:
            self._thread.join(timeout=2.0)
        for handle in (self._wake_r, self._wake_w):
            try:
                os.close(handle)
            except OSError:  # pragma: no cover
                pass

    def set_busy(self, busy: bool) -> None:
        """Called around gestures: no captures while the overlay is moving."""
        if busy:
            self._busy.set()
        else:
            self._busy.clear()

    @property
    def busy(self) -> bool:
        """True while a gesture is on screen: nothing heavy should run now."""
        return self._busy.is_set()

    @property
    def last_damage_at(self) -> float:
        """When the screen last changed, for callers deciding whether to look."""
        return self._last_damage_at

    def refresh_soon(self, urgent: bool = False) -> None:
        """Ask for a fresh snapshot (focus changed, window moved, ...).

        ``urgent`` is for changes that invalidate the whole picture -- a
        workspace switch -- where waiting the usual couple of seconds for the
        screen to hold still would leave the strip showing the old desktop.
        """
        if urgent:
            self._urgent.set()
        self._dirty.set()
        with self._lock:
            self._requests += 1
        try:
            os.write(self._wake_w, b"\x01")
        except OSError:  # pragma: no cover - the pipe is only closed at shutdown
            pass

    def set_targets(self, targets: Sequence[WindowTarget], desktop: int = -1) -> None:
        """Which windows to snapshot on their own, and which desktop they are on.

        The desktop is stamped onto the snapshot that follows, so the app can
        tell whether this screen grab is still the desktop it is cropping
        windows out of: a crop of the *previous* workspace looks exactly like a
        plausible card, and it is wrong.
        """
        with self._lock:
            self._targets = tuple(targets)
            self._desktop = int(desktop)

    @property
    def snapshot_desktop(self) -> int:
        with self._lock:
            return self._captured_desktop

    def snapshot(
        self,
    ) -> tuple[Image.Image, Image.Image, dict[int, Image.Image], int] | None:
        """(screen, backdrop, per-window pixels, desktop); None before a grab.

        The images are handed over as they are, not copied. The worker replaces
        them wholesale under the lock and never edits one in place, and every
        consumer only reads from them (crop, scale, paste), so the copies that
        used to be here were two full-screen memcpys -- ~12MB per capture, done
        on the thread whose job is to stay out of the swipe's way.
        """
        with self._lock:
            if self._plain is None or self._backdrop is None:
                return None
            return (
                self._plain,
                self._backdrop,
                dict(self._windows),
                self._captured_desktop,
            )

    @property
    def generation(self) -> int:
        with self._lock:
            return self._generation

    @property
    def age(self) -> float:
        with self._lock:
            return time.monotonic() - self._captured_at if self._captured_at else float("inf")

    @property
    def stats(self) -> dict:
        # One lock acquisition only: `age` takes the same lock, so calling it
        # from in here would deadlock the caller.
        with self._lock:
            age = time.monotonic() - self._captured_at if self._captured_at else None
            return {
                "capture_ms": round(self._capture_ms, 1),
                "age_s": round(age, 1) if age is not None else None,
                "generation": self._generation,
                "geometry": self._geometry,
                "error": self._last_error,
                "xdamage": self._damage_id is not None,
                "windows": len(self._windows),
                "desktop": self._captured_desktop,
                "requests": self._requests,
            }

    # -- worker ------------------------------------------------------------
    def _run(self) -> None:
        try:
            self._display = xdisplay.Display()
        except Exception as exc:  # pragma: no cover
            with self._lock:
                self._last_error = f"cannot open display: {exc}"
            return
        # python-xlib's default handler prints every protocol error to stderr.
        # We only care about the pixel data, so keep them quiet and remember
        # the last one for the debug output.
        self._display.set_error_handler(self._on_x_error)
        self._root = self._display.screen().root
        self._arm()
        self._capture()  # so the very first swipe already has pixels

        while not self._stop.is_set():
            self._await_change()
            self._settle_and_capture()

    def _on_x_error(self, error, request=None) -> None:
        with self._lock:
            self._last_error = f"{type(error).__name__}: {error}"

    def _arm(self) -> None:
        if not _HAVE_DAMAGE or self._damage_id is not None or self._display is None:
            return
        try:
            self._display.query_extension("DAMAGE")
            self._damage_id = self._root.damage_create(_damage.DamageReportNonEmpty)
            self._display.flush()
        except Exception as exc:  # pragma: no cover
            self._damage_id = None
            with self._lock:
                self._last_error = f"damage unavailable: {exc}"

    def _disarm(self) -> None:
        if self._damage_id is None or self._display is None:
            return
        damage_id, self._damage_id = self._damage_id, None
        try:
            self._display.damage_destroy(damage_id)
            self._display.flush()
        except Exception:  # pragma: no cover
            pass

    def _await_change(self) -> None:
        """Block until the screen changes, or the fallback poll interval."""
        if self._display is None:
            self._stop.wait(1.0)
            return
        if self._damage_id is None:
            # No DAMAGE available: poll on a slow timer instead of watching.
            if self._stop.wait(self.config.capture_poll_s):
                return
            self._note_damage()
            return
        try:
            ready, _, _ = select.select(
                [self._display.fileno(), self._wake_r], [], [], 0.5
            )
        except (OSError, ValueError):  # pragma: no cover
            self._stop.wait(0.5)
            return
        if self._wake_r in ready:
            try:
                os.read(self._wake_r, 64)
            except OSError:  # pragma: no cover
                pass
            if self._display.fileno() not in ready:
                # Only a refresh was asked for: nothing to drain.
                return
        if not ready:
            return
        try:
            while self._display.pending_events():
                event = self._display.next_event()
                if _HAVE_DAMAGE and isinstance(event, _damage.DamageNotify):
                    self._note_damage()
                    self._disarm()
        except XError:  # pragma: no cover
            pass

    def _note_damage(self) -> None:
        self._last_damage_at = time.monotonic()
        self._dirty.set()

    def _settle_and_capture(self) -> None:
        if not self._dirty.is_set():
            return
        first_pending = time.monotonic()
        while not self._stop.is_set():
            if self._busy.is_set():
                # A gesture is on screen: never grab pixels right now.
                time.sleep(0.05)
                first_pending = time.monotonic()
                continue
            now = time.monotonic()
            settled = self._last_damage_at and now - self._last_damage_at >= self.config.capture_debounce_s
            limit = URGENT_SETTLE_WAIT if self._urgent.is_set() else MAX_SETTLE_WAIT
            waited_too_long = now - first_pending >= limit
            if settled or waited_too_long:
                break
            time.sleep(0.02)
        self._dirty.clear()
        self._urgent.clear()
        if self._busy.is_set():
            self._dirty.set()
            return
        self._capture()

    # -- the actual work ---------------------------------------------------
    def _capture(self) -> bool:
        if self._display is None or self._root is None:
            return False
        try:
            root = self._root.get_geometry()
            width, height = root.width, root.height
            started = time.monotonic()
            # Read the workspace *before* the pixels: the stamp has to describe
            # the moment the grab started. A switch that lands while the grab
            # is in flight leaves a picture of the old workspace stamped with
            # the old workspace, which is exactly what the app needs to know in
            # order to refuse to crop it.
            with self._lock:
                requested = self._desktop
            observed = read_current_desktop(self._display, self._root)
            plain = grab_screen(self._display, self._root)
            if plain is None:
                raise ValueError("could not read the root window")
            windows = self._read_windows()
            backdrop = self._make_backdrop(plain)
            elapsed = (time.monotonic() - started) * 1000.0
        except (XError, ValueError, OSError) as exc:
            with self._lock:
                self._last_error = str(exc)
            return False

        with self._lock:
            self._plain = plain
            self._backdrop = backdrop
            self._windows = windows
            self._captured_desktop = stamped_desktop(observed, requested)
            self._geometry = (0, 0, width, height)
            self._captured_at = time.monotonic()
            self._capture_ms = elapsed
            self._generation += 1
            self._last_error = None

        self._arm()  # re-arm only once we have taken what we came for
        if self._on_update:
            try:
                self._on_update(self._generation)
            except Exception:
                pass
        return True

    def _read_windows(self) -> dict[int, Image.Image]:
        """Each window's own pixels, for the cards.

        Anything that cannot be read here is simply left out: the overlay then
        crops that window out of the screen grab, which is correct as long as
        nothing overlaps it.
        """
        if self._display is None:
            return {}
        with self._lock:
            targets = tuple(self._targets)
        out: dict[int, Image.Image] = {}
        for target in targets:
            if self._busy.is_set():
                # A gesture came up: the rest can wait for the next capture,
                # which the app asks for as soon as the swipe is over. Each
                # read is ~25ms of X and PIL that a swipe should not sit behind.
                break
            picture = read_window_pixels(self._display, target.frame)
            if picture is not None:
                out[int(target.xid)] = picture
        if not out and self._windows:
            # Nothing could be read this time -- the compositor is between
            # redirects, say. Handing back an empty set would throw away every
            # card the overlay has; the previous pixels are a fraction of a
            # second old and still this window.
            return dict(self._windows)
        return out

    def _make_backdrop(self, plain: Image.Image) -> Image.Image:
        """Blur, dim and desaturate -- all at 1/scale, then one upscale.

        Doing the per-pixel work on the tiny copy instead of the full frame is
        what keeps this around ~20ms rather than a few hundred.
        """
        scale = max(2, int(self.config.blur_downscale))
        width, height = plain.size
        small = plain.resize((max(1, width // scale), max(1, height // scale)), Image.BILINEAR)
        small = small.filter(ImageFilter.GaussianBlur(self.config.blur_radius))
        if self.config.dim > 0:
            black = Image.new("RGB", small.size, (0, 0, 0))
            small = Image.blend(small, black, min(1.0, max(0.0, self.config.dim)))
        # A little less colour makes the backdrop read as a background rather
        # than a second, blurrier copy of the desktop.
        small = ImageEnhance.Color(small).enhance(0.9)
        return small.resize((width, height), Image.BICUBIC)
