"""Which apps are open in this workspace, and in what order.

The filtering / grouping / ordering rules live in pure functions so they can be
tested without an X server. `WindowManager` is the thin X11 layer around them:
it enumerates windows, watches for focus changes and activates the chosen one.

Scope rules (the gesture only ever moves inside the current workspace):

* a window belongs to the workspace when its ``_NET_WM_DESKTOP`` matches the
  current desktop, or when it is sticky (visible everywhere)
* desktop windows (nemo-desktop), docks/panels and skip-taskbar windows are
  never listed, and neither is our own overlay
* minimized windows stay in the list, because they are still "open here" --
  activating one restores it, exactly like Windows 11
"""

from __future__ import annotations

import dataclasses
import os
import select
import time
from collections.abc import Iterable, Sequence

from Xlib import X, Xatom, display as xdisplay
from Xlib.error import XError
from Xlib.protocol import event as xevent

STICKY_DESKTOP = 0xFFFFFFFF
_UNRANKED = 1 << 30
#: "The caller did not pass this in" -- None is a real answer for a position.
_MISSING = object()

#: How long a window enumeration is reused for. Reading the list is a round trip
#: per property per window -- measured at ~50ms on an ordinary desktop -- and it
#: happens on the GTK thread, twice in a row, exactly while the strip is
#: opening. Half a second collapses those two passes into one and is far shorter
#: than anything a person can notice.
WINDOW_CACHE_S = 0.5


@dataclasses.dataclass(frozen=True)
class WinInfo:
    """Everything we need to know about one window."""

    xid: int
    wm_class: str = ""
    pid: int | None = None
    title: str = ""
    x: int = 0
    y: int = 0
    width: int = 0
    height: int = 0
    desktop: int = -1
    minimized: bool = False
    skip_taskbar: bool = False
    dock: bool = False
    desktop_window: bool = False
    our_own: bool = False
    normal: bool = True
    # The window the client was reparented into (its titlebar frame), or the
    # window itself when it is a direct child of the root. Its pixels are the
    # window's own, even when something else is in front.
    frame: int = 0
    # False when we could not work out where the window is on screen. The
    # window is still listed and still switchable, but it must not be cropped
    # out of a screen grab: window-relative coordinates would show the wrong
    # pixels entirely (this is what made every preview look like a random
    # screenshot).
    placed: bool = True

    @property
    def rect(self) -> tuple[int, int, int, int]:
        return (self.x, self.y, self.width, self.height)

    @property
    def key(self) -> str:
        """Grouping key: an app is one entry, like Windows 11."""
        if self.wm_class:
            return self.wm_class
        if self.pid:
            return f"pid:{self.pid}"
        return f"xid:{self.xid}"


@dataclasses.dataclass(frozen=True)
class WindowRef:
    xid: int
    title: str
    rect: tuple[int, int, int, int]
    minimized: bool = False


@dataclasses.dataclass
class AppEntry:
    key: str
    label: str
    windows: list[WindowRef]

    @property
    def top(self) -> WindowRef:
        return self.windows[0]


# ---------------------------------------------------------------------------
# Pure model
# ---------------------------------------------------------------------------


def usable_windows(
    windows: Iterable[WinInfo],
    current_desktop: int,
    our_pid: int | None = None,
) -> list[WinInfo]:
    """Windows that belong to this workspace and belong in the switcher."""
    keep: list[WinInfo] = []
    for win in windows:
        if win.desktop_window or win.dock or win.skip_taskbar or not win.normal:
            continue
        if win.our_own or (our_pid is not None and win.pid == our_pid):
            continue
        if win.desktop != -1 and win.desktop != current_desktop:
            continue
        keep.append(win)
    return keep


def build_app_list(
    windows: Iterable[WinInfo],
    mru: Sequence[int],
    current_desktop: int,
    group_by_app: bool = True,
    our_pid: int | None = None,
    labels: dict[str, str] | None = None,
) -> list[AppEntry]:
    """Ordered entries, current workspace only, in the order of ``mru``.

    ``mru`` is normally "whatever order we want, then every window" so that
    windows nobody ranked still appear. A window can be in both halves, and the
    **first** mention has to win: a plain dict comprehension takes the last one,
    which quietly threw the ranking away and parked the focused window at the
    far end of every strip.
    """
    rank: dict[int, int] = {}
    for index, xid in enumerate(mru):
        rank.setdefault(int(xid), index)
    wins = usable_windows(windows, current_desktop, our_pid)
    wins.sort(key=lambda w: rank.get(w.xid, _UNRANKED))

    groups: dict[str, AppEntry] = {}
    order: list[str] = []
    for win in wins:
        key = win.key if group_by_app else f"xid:{win.xid}"
        ref = WindowRef(win.xid, win.title, win.rect, win.minimized)
        entry = groups.get(key)
        if entry is None:
            label = (labels or {}).get(key) or _pretty_label(win.wm_class or win.title)
            entry = AppEntry(key=key, label=label, windows=[])
            groups[key] = entry
            order.append(key)
        entry.windows.append(ref)
    return [groups[key] for key in order]


def entry_index(entries: Sequence[AppEntry], xid: int | None) -> int:
    """Which card the focused window belongs to.

    The outline the strip opens on must be the card ``activate()`` would pick,
    so both come from this one rule. Falls back to the first card when the
    focused window is not in the list (a panel, a dialog, nothing).
    """
    if xid is None:
        return 0
    for position, entry in enumerate(entries):
        if any(w.xid == xid for w in entry.windows):
            return position
    return 0


def strip_priority(mode: str, mru_order: Sequence[int], mapping_order: Sequence[int]) -> list[int]:
    """Which order the cards are laid out in, left to right.

    ``"recent"`` -- the default -- re-sorts by focus history, like Alt+Tab: the
    window you are on is card 0, so a swipe always starts from where you are and
    the row re-arranges itself after every switch to keep it that way.

    ``"opened"`` follows EWMH's mapping order instead, the order the windows
    were opened. That keeps the row *still* between gestures, at the cost of the
    focused window being able to sit anywhere in it (so a closed and reopened
    window jumps to the end).
    """
    if mode == "recent":
        return list(mru_order)
    return list(mapping_order)


def _pretty_label(raw: str) -> str:
    if not raw:
        return "?"
    name = raw.split(".")[-1] if "." in raw else raw
    return name.replace("-", " ").strip().title()


class MruTracker:
    """Most-recently-used window order.

    Seeded from the stacking order and then kept honest by focus events, so the
    swipes stay predictable even after you click around or change workspace.
    """

    def __init__(self, initial: Sequence[int] = ()):
        # `initial` is most-recent-first, so it is taken as given rather than
        # pushed through note() (which would reverse it).
        self._order: list[int] = list(dict.fromkeys(int(x) for x in initial if x))

    def note(self, xid: int) -> None:
        if not xid:
            return
        try:
            self._order.remove(xid)
        except ValueError:
            pass
        self._order.insert(0, xid)
        del self._order[256:]

    def forget(self, xid: int) -> None:
        try:
            self._order.remove(xid)
        except ValueError:
            pass

    def rank(self, xid: int) -> int:
        try:
            return self._order.index(xid)
        except ValueError:
            return _UNRANKED

    @property
    def order(self) -> list[int]:
        return list(self._order)

    def seed_from_stacking(self, stacking: Sequence[int]) -> None:
        """Learn about windows we have not seen, newest (topmost) first.

        `_NET_CLIENT_LIST_STACKING` is ordered bottom-to-top, so reversing it
        yields most-recently-used first. Windows we already know keep their
        place at the front, which is what makes focus history stick.
        """
        known = set(self._order)
        for xid in reversed(list(stacking)):
            if xid and xid not in known:
                self._order.append(int(xid))
                known.add(int(xid))
        del self._order[256:]


def rects_overlap(a: Sequence[int], b: Sequence[int]) -> bool:
    """Do two (x, y, w, h) rectangles share any pixels?"""
    return (
        a[0] < b[0] + b[2]
        and b[0] < a[0] + a[2]
        and a[1] < b[1] + b[3]
        and b[1] < a[1] + a[3]
    )


def windows_needing_own_pixels(
    windows: Sequence[WinInfo],
    current_desktop: int,
    bounds: tuple[int, int, int, int],
    our_pid: int | None = None,
) -> set[int]:
    """Windows a crop of the screen would misrepresent.

    A card is the screen at the window's rect, which is exactly right and costs
    nothing extra -- the screen is already grabbed -- as long as the window is
    the frontmost thing there and wholly on screen. Anything else has to be read
    from the compositor instead, which is 20-odd milliseconds per window, so
    only the windows that need it pay for it.

    ``windows`` must be in stacking order, bottom to top: a window is covered
    by whatever comes after it.
    """
    left, top, right, bottom = bounds
    shareable = [
        win
        for win in windows
        if not win.minimized
        and not win.our_own
        and (win.desktop == -1 or win.desktop == current_desktop)
        and not (our_pid is not None and win.pid == our_pid)
    ]
    need: set[int] = set()
    for index, win in enumerate(shareable):
        x, y, width, height = win.rect
        if x < left or y < top or x + width > right or y + height > bottom:
            # Partly off screen: a crop would come out padded, or empty.
            need.add(win.xid)
            continue
        for other in shareable[index + 1 :]:
            if rects_overlap(win.rect, other.rect):
                need.add(win.xid)
                break
    return need


def root_position(root, window) -> tuple[int, int] | None:
    """Where a window's top-left corner sits on the root window.

    ``get_geometry()`` answers in the window's *parent*: for a window the manager
    has reparented, that is a few pixels inside its titlebar frame, so cropping
    a screen grab there shows a slice of the top-left of the screen instead of
    the window -- the reason every preview once looked like a random
    screenshot.

    Two easy ways to get this wrong, both of which this function exists to make
    impossible, are checked by tests: python-xlib's ``translate_coords`` is a
    method on the *destination* window (so it is asked of the root, not of the
    window), and the reply's fields are named ``x``/``y``. When the answer
    cannot be established this returns None, and callers must refuse to guess
    rather than fall back to parent-relative coordinates.
    """
    try:
        translated = root.translate_coords(window, 0, 0)
        return int(translated.x), int(translated.y)
    except (XError, AttributeError, TypeError):
        return None


def frame_of(root, window) -> int:
    """The window that actually holds this window's pixels.

    The window manager reparents clients into a titlebar frame, and the
    compositor only redirects the root's own children, so the frame is the
    drawable with the client's real content. A window that is already a child
    of the root is its own frame.
    """
    try:
        parent = window.query_tree().parent
    except (XError, AttributeError):
        return 0
    parent_id = int(getattr(parent, "id", 0) or 0)
    window_id = int(getattr(window, "id", 0) or 0)
    if not parent_id or parent_id == int(getattr(root, "id", 0) or 0):
        return window_id
    return parent_id


# ---------------------------------------------------------------------------
# X11 layer
# ---------------------------------------------------------------------------

#: EWMH's "this window is on every desktop" value for ``_NET_WM_DESKTOP``.
ALL_DESKTOPS = 0xFFFFFFFF


def make_sticky(display, root, xid: int) -> bool:
    """Ask the window manager to show a window of ours on **every** workspace.

    A window manager decides which workspace a window lives on when it first
    maps it, and a window it has placed on one workspace is not composited on
    any other -- so an overlay that does not say otherwise is invisible
    everywhere except the workspace winTab was started in, while the gesture
    quietly keeps working. That is the whole bug this exists to prevent.

    Two requests, and the caller makes both (before the map and again after
    it), because they cover different moments:

    * ``_NET_WM_DESKTOP = 0xFFFFFFFF`` is read at first map, and is what a
      window created once at startup and never unmapped needs
    * the ``_NET_WM_STATE`` message adding ``_NET_WM_STATE_STICKY`` is what
      ``wmctrl -b add,sticky`` sends, and is the one that actually moves a
      window -- but only one the manager has already managed

    (``Gdk.Window.stick()`` looks like the natural call and is not: under
    Muffin it changes no property, sends nothing, and leaves the window on its
    one workspace. Measured, not assumed.)
    """
    try:
        window = display.create_resource_object("window", int(xid))
        window.change_property(
            display.intern_atom("_NET_WM_DESKTOP"), Xatom.CARDINAL, 32, [ALL_DESKTOPS]
        )
        message = xevent.ClientMessage(
            window=int(xid),
            client_type=display.intern_atom("_NET_WM_STATE"),
            # [action = add, first property, second property, source = app]
            data=(32, [1, display.intern_atom("_NET_WM_STATE_STICKY"), 0, 1, 0]),
        )
        root.send_event(
            message, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask
        )
        display.flush()
    except (XError, AttributeError, TypeError, ValueError):
        return False
    return True


def clear_attention(display, root, xid: int) -> bool:
    """Withdraw ``_NET_WM_STATE_DEMANDS_ATTENTION`` from one of our windows.

    The overlay is a full-screen, sticky, keep-above window that stays mapped
    for the whole session, and Cinnamon stamps every such ``NOTIFICATION``
    window with the attention flag when it first manages it -- as if it were a
    notification bubble waiting to be read. An unread flag sitting on screen
    permanently is poison for everything the manager does next: its
    focus-stealing prevention sees "a window wants attention" every time
    another application maps a new window, and routes that window into
    attention-flashing instead of raising and focusing it -- so the newly
    opened program sits behind the one you were using until you click it.

    The flag has nothing to do with what a window *will* draw; it is set at
    manage time regardless of opacity, so it cannot be designed away. The EWMH
    way to withdraw it is the ``_NET_WM_STATE`` client message with action 0
    (remove), naming the flag, with source indication 1 ("from the
    application", i.e. from us on our own window). A window manager honours
    that for a window it manages, which is exactly what ours is by then.
    """
    try:
        message = xevent.ClientMessage(
            window=int(xid),
            client_type=display.intern_atom("_NET_WM_STATE"),
            # [action = remove, first property, second property, source = app]
            data=(32, [0, display.intern_atom("_NET_WM_STATE_DEMANDS_ATTENTION"), 0, 1, 0]),
        )
        root.send_event(
            message, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask
        )
        display.flush()
    except (XError, AttributeError, TypeError, ValueError):
        return False
    return True


class WindowManager:
    def __init__(self, config):
        self.config = config
        self.display = xdisplay.Display()
        # Keep protocol errors (windows vanishing mid-scan, odd extension
        # queries) from spamming stderr; every read below is defensive.
        self.display.set_error_handler(self._on_x_error)
        self.root = self.display.screen().root
        self.mru = MruTracker()
        self._errors: list[str] = []
        self._atoms: dict[str, int] = {}
        # Caches, because the gesture path cannot afford round trips:
        #   _atom_text  atom id -> name. A name never changes once an atom
        #               exists, and this is asked for every state and type atom
        #               of every window on every pass, plus every property
        #               event the window manager sends.
        #   _static     xid -> (WM_CLASS, pid). Both are set before the window
        #               is mapped and cannot change without the window being
        #               replaced; pruned to the windows that still exist.
        #   _windows    the last enumeration, reused for WINDOW_CACHE_S.
        self._atom_text: dict[int, str] = {}
        self._static: dict[int, tuple[str, int | None]] = {}
        self._windows: list[WinInfo] = []
        self._windows_at = 0.0
        self._own_pid = os.getpid()
        # Anything we do not understand falls back to the stable row; the
        # config loader warns about it, this just refuses to crash.
        mode = str(getattr(config, "order", "recent"))
        self.order_mode = mode if mode in ("opened", "recent") else "recent"
        self._last_event_time = 0
        self._watching = False
        self.mru.seed_from_stacking(self.stacking_order())

    def _on_x_error(self, error, request=None) -> None:
        self._errors.append(f"{type(error).__name__}: {error}")
        del self._errors[:-16]

    # -- atoms -------------------------------------------------------------
    def atom(self, name: str) -> int:
        atom = self._atoms.get(name)
        if atom is None:
            atom = self.display.intern_atom(name)
            self._atoms[name] = atom
        return atom

    @staticmethod
    def _text(value) -> str:
        """python-xlib hands back str or bytes depending on the property."""
        if value is None:
            return ""
        if isinstance(value, bytes):
            return value.decode("utf-8", "replace")
        if isinstance(value, str):
            return value
        return str(value)

    def _atom_text_of(self, atom: int) -> str:
        """The name of an atom, remembered -- an atom's name never changes.

        Reading it is a round trip, and this runs for every type and state atom
        of every window on every pass, so re-reading them is a measurable slice
        of the strip's opening latency.
        """
        key = int(atom)
        name = self._atom_text.get(key)
        if name is not None:
            return name
        try:
            name = self._text(self.display.get_atom_name(key))
        except (XError, AttributeError, TypeError):
            return ""
        self._atom_text[key] = name
        return name

    def _atom_names(self, value) -> set[str]:
        return {name for name in (self._atom_text_of(raw) for raw in value or []) if name}

    def _prop(self, win, name: str, default=None):
        try:
            prop = win.get_full_property(self.atom(name), X.AnyPropertyType)
        except XError:
            return default
        if prop is None or not len(prop.value):
            return default
        return prop.value

    # -- screen ------------------------------------------------------------
    def screen_geometry(self) -> tuple[int, int, int, int]:
        root = self.root.get_geometry()
        return (0, 0, root.width, root.height)

    def monitor_geometry(self, xid: int | None = None) -> tuple[int, int, int, int]:
        """Monitor containing `xid`, else the monitor with the pointer.

        Single-monitor setups fall straight through to the whole screen.
        """
        if xid:
            try:
                monitor = self.display.get_monitor_at_window(
                    self.display.create_resource_object("window", xid)
                )
                info = monitor.get_geometry()
                return (info.x, info.y, info.width, info.height)
            except (XError, AttributeError):
                pass
        return self.screen_geometry()

    def current_desktop(self) -> int:
        value = self._prop(self.root, "_NET_CURRENT_DESKTOP")
        if not value:
            return 0
        try:
            return int(value[0])
        except (TypeError, ValueError):
            return 0

    def desktop_count(self) -> int:
        """How many workspaces exist (at least one)."""
        value = self._prop(self.root, "_NET_NUMBER_OF_DESKTOPS")
        try:
            count = int(value[0]) if value else 1
        except (TypeError, ValueError):
            count = 1
        return max(1, count)

    def stacking_order(self) -> list[int]:
        """Bottom-to-top list of managed windows."""
        value = self._prop(self.root, "_NET_CLIENT_LIST_STACKING")
        if value is None:
            value = self._prop(self.root, "_NET_CLIENT_LIST") or []
        return [int(x) for x in value]

    def client_list_order(self) -> list[int]:
        """Windows in the order they were opened.

        Unlike the stacking order this does not change when you click something,
        so the strip's cards stay where they are.
        """
        value = self._prop(self.root, "_NET_CLIENT_LIST")
        if value:
            return [int(x) for x in value]
        return list(reversed(self.stacking_order()))

    def active_window(self) -> int | None:
        value = self._prop(self.root, "_NET_ACTIVE_WINDOW")
        if not value:
            return None
        xid = int(value[0])
        return xid or None

    # -- enumeration -------------------------------------------------------
    def list_windows(self, force: bool = False) -> list[WinInfo]:
        """Every managed window with the properties the switcher cares about.

        The answer is reused for ``WINDOW_CACHE_S``. Opening the strip asks for
        it twice in a row -- once to publish what the capture thread should
        read, once to build the cards -- and each pass costs a round trip per
        property per window on the GTK thread, which is time the strip spends
        not being on screen. ``force=True`` is for the passes whose job *is* to
        notice a window that moved.
        """
        now = time.monotonic()
        if not force and self._windows and now - self._windows_at < WINDOW_CACHE_S:
            return list(self._windows)
        out: list[WinInfo] = []
        for xid in self.stacking_order():
            info = self._describe(xid)
            if info is not None:
                out.append(info)
        self._windows = out
        self._windows_at = now
        self._forget_missing(out)
        return list(out)

    def invalidate(self) -> None:
        """A window came or went: the next read must not come from the cache."""
        self._windows_at = 0.0

    def _forget_missing(self, windows: Iterable[WinInfo]) -> None:
        """Drop per-window facts for windows that are gone, so an id that gets
        recycled cannot show a dead window's name."""
        alive = {win.xid for win in windows}
        for xid in [xid for xid in self._static if xid not in alive]:
            del self._static[xid]

    def _describe(self, xid: int) -> WinInfo | None:
        try:
            win = self.display.create_resource_object("window", xid)
            attributes = win.get_attributes()
            if attributes is None or attributes.override_redirect:
                return None

            types = self._atom_names(self._prop(win, "_NET_WM_WINDOW_TYPE"))
            states = self._atom_names(self._prop(win, "_NET_WM_STATE"))
            desktop = self._prop(win, "_NET_WM_DESKTOP")
            if desktop:
                desktop_id = int(desktop[0])
            else:
                # No opinion from the window (freshly mapped, or the desktop
                # window itself): treat it as visible here rather than hiding
                # it because it happens to lack the property.
                desktop_id = -1
            if desktop_id == STICKY_DESKTOP:
                desktop_id = -1

            wm_class, pid = self._static_facts(xid, win)
            title = self._read_title(win)
            # The window's place on screen is asked for once and used for both
            # the rect and "can we place it at all": asking twice cost a round
            # trip per window for the same answer.
            position = self._root_position(win)
            x, y, width, height = self._read_frame_geometry(win, position)
            placed = position is not None
            frame = self._frame_of(win)
        except XError:
            return None

        return WinInfo(
            xid=xid,
            wm_class=wm_class,
            pid=pid,
            title=title,
            x=x,
            y=y,
            width=width,
            height=height,
            desktop=desktop_id,
            minimized="_NET_WM_STATE_HIDDEN" in states,
            skip_taskbar="_NET_WM_STATE_SKIP_TASKBAR" in states
            or "_NET_WM_STATE_SKIP_PAGER" in states,
            dock="_NET_WM_WINDOW_TYPE_DOCK" in types,
            desktop_window="_NET_WM_WINDOW_TYPE_DESKTOP" in types,
            our_own=pid == self._own_pid,
            normal="_NET_WM_WINDOW_TYPE_NORMAL" in types or not types,
            frame=frame,
            placed=placed,
        )

    def _static_facts(self, xid: int, win) -> tuple[str, int | None]:
        """A window's class and pid, read once per window and remembered.

        Both are set before the window is mapped and cannot change without the
        window being replaced, so re-reading them on every pass is two round
        trips per window for an answer we already have.
        """
        facts = self._static.get(xid)
        if facts is None:
            pid_prop = self._prop(win, "_NET_WM_PID")
            facts = (self._read_wm_class(win), int(pid_prop[0]) if pid_prop else None)
            self._static[xid] = facts
        return facts

    def _read_wm_class(self, win) -> str:
        raw = self._prop(win, "WM_CLASS")
        if not raw:
            return ""
        if isinstance(raw, bytes):
            # WM_CLASS is "instance\0class\0"; the class half is the stable
            # name to group apps by (instance changes per window/toolkit).
            parts = [p for p in raw.split(b"\0") if p]
            return (parts[-1] if parts else b"").decode("latin-1").strip().lower()
        return self._text(raw).strip().strip('"').lower()

    def _read_title(self, win) -> str:
        for name in ("_NET_WM_NAME", "WM_NAME"):
            raw = self._prop(win, name)
            if raw:
                return self._text(raw).strip('"')
        return ""

    def _root_position(self, win) -> tuple[int, int] | None:
        return root_position(self.root, win)

    def _is_placed(self, win) -> bool:
        return root_position(self.root, win) is not None

    def _frame_of(self, win) -> int:
        return frame_of(self.root, win)

    def _read_frame_geometry(self, win, position=_MISSING) -> tuple[int, int, int, int]:
        """Root-relative rect including the titlebar decoration.

        ``position`` may be passed in by :meth:`_describe`, which needs the same
        answer to decide whether the window can be placed at all -- one lookup
        instead of two, on the path that opens the strip.
        """
        geometry = win.get_geometry()
        if geometry is None:
            return (0, 0, 0, 0)
        if position is _MISSING:
            position = self._root_position(win)
        x, y = position if position is not None else (geometry.x, geometry.y)

        extents = self._prop(win, "_NET_FRAME_EXTENTS")
        if extents and len(extents) >= 4:
            left, right, top, bottom = (int(v) for v in extents[:4])
        else:
            left = right = top = bottom = 0
        return (
            x - left,
            y - top,
            geometry.width + left + right,
            geometry.height + top + bottom,
        )

    # -- actions -----------------------------------------------------------
    def activate(self, xid: int) -> None:
        """Focus, unminimize and raise a window, the way a pager asks for it.

        Timestamp note, because this was a real bug: EWMH asks for the time of
        the user action, and window managers quietly ignore a request whose
        timestamp is older than the last one they saw -- focus-stealing
        prevention. Quoting the time of the last property change we happened to
        notice looks reasonable and is wrong: minutes later the request is
        refused, nothing changes, so no new property change arrives to refresh
        that timestamp, and switching stops working for good. We read raw evdev
        touches, so there is no X input event to quote; the gesture is a user
        action happening now, so ``CurrentTime`` -- which the window manager
        resolves to the current server time -- is the honest answer.
        """
        try:
            win = self.display.create_resource_object("window", xid)
            message = xevent.ClientMessage(
                window=win,
                client_type=self.atom("_NET_ACTIVE_WINDOW"),
                data=(32, [2, X.CurrentTime, 0, 0, 0]),
            )
            self.root.send_event(
                message,
                event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask,
            )
            self.display.flush()
        except XError as exc:
            print(f"wintab: could not activate 0x{xid:x}: {exc}")

    def make_sticky(self, xid: int) -> bool:
        """Put one of our own windows -- the overlay -- on every workspace."""
        return make_sticky(self.display, self.root, xid)

    def clear_attention(self, xid: int) -> bool:
        """Take the attention flag off one of our windows (the overlay) again.

        Needed right after the map, when Cinnamon stamps the flag on, and after
        every sticky re-assertion, which re-runs the state machinery that set
        it. Existing flags on other applications' windows are never touched:
        the request is sent only for our own xid.
        """
        return clear_attention(self.display, self.root, xid)

    def refresh_mru(self) -> None:
        self.mru.seed_from_stacking(self.stacking_order())

    def build(self) -> tuple[list[AppEntry], int]:
        """Current app list plus the index of the focused app."""
        windows = self.list_windows()
        priority = strip_priority(self.order_mode, self.mru.order, self.client_list_order())
        entries = build_app_list(
            windows,
            priority + [w.xid for w in windows],
            self.current_desktop(),
            group_by_app=self.config.group_by_app,
            our_pid=self._own_pid,
        )
        return entries, entry_index(entries, self.active_window())

    # -- events ------------------------------------------------------------
    def watch_focus(self) -> None:
        if self._watching:
            return
        self.root.change_attributes(event_mask=X.PropertyChangeMask)
        self.display.flush()
        self._watching = True

    def fileno(self) -> int:
        return self.display.fileno()

    def drain_events(self, limit: int = 128) -> None:
        """Read pending X events (called from the GTK loop's io watch)."""
        changed_focus = False
        changed_desktop = False
        for _ in range(limit):
            if not self.display.pending_events():
                try:
                    ready, _, _ = select.select([self.display.fileno()], [], [], 0)
                except (OSError, ValueError):
                    break
                if not ready:
                    break
            try:
                ev = self.display.next_event()
            except XError:
                continue
            if getattr(ev, "type", None) != X.PropertyNotify:
                continue
            self._last_event_time = getattr(ev, "time", self._last_event_time) or self._last_event_time
            name = self._atom_name_of(getattr(ev, "atom", 0))
            if name == "_NET_ACTIVE_WINDOW":
                active = self.active_window()
                if active:
                    self.mru.note(active)
                    changed_focus = True
            elif name == "_NET_CLIENT_LIST":
                self.invalidate()
                self.refresh_mru()
            elif name == "_NET_CURRENT_DESKTOP":
                # A workspace switch is not always accompanied by a focus
                # change (switch to an empty workspace), and it invalidates
                # every pixel we are holding, so it gets its own hook.
                changed_desktop = True
        if changed_focus:
            self.on_focus_change()
        if changed_desktop:
            self.on_desktop_change()

    def _atom_name_of(self, atom: int) -> str:
        """The name of a property atom, cached: this runs on every property
        event the window manager sends us."""
        return self._atom_text_of(atom)

    def on_focus_change(self) -> None:
        """Hook for the app to refresh its capture cache."""

    def on_desktop_change(self) -> None:
        """Hook for the app to drop pixels that belonged to the old workspace."""
