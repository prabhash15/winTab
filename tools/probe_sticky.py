#!/usr/bin/env python3
"""Is the strip actually *drawn* after changing workspace?

The gesture works everywhere; the strip is what went missing. Two things can
hide it, and this tool measures both:

* the overlay is a window like any other, so Muffin composites it only on the
  workspace it was first mapped on -- unless it is sticky
  (``_NET_WM_DESKTOP = 0xFFFFFFFF``). The property is read back off the live
  window here, so a wrong value is visible without switching anything.
* a window that *is* on the right workspace can still be drawn wrong. So the
  strip is opened with placeholder cards on the current workspace and on the
  next one, and the screen is compared with and without it: a missing strip is
  a difference of about zero. (Placeholders are enough precisely because the
  question is whether the overlay is composited at all, not what is on its
  cards -- which is what ``probe_switch.py`` covers.)

    python3 tools/probe_sticky.py            # current workspace only
    python3 tools/probe_sticky.py --switch   # then the next one, and back
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gtk  # noqa: E402

from Xlib import X  # noqa: E402
from Xlib.error import XError  # noqa: E402

from wintab import capture, config as config_module  # noqa: E402
from wintab.apps import AppEntry, WindowManager, WindowRef  # noqa: E402
from wintab.overlay import Carousel  # noqa: E402

ALL_DESKTOPS = 0xFFFFFFFF

#: How much of the screen has to change for the strip to count as drawn. The
#: overlay is opaque and covers everything, so a composited strip changes almost
#: every pixel; one that is not composited changes none. The *amount* each pixel
#: changes is not a good measure -- a dark desktop is close to the overlay's own
#: dark background -- so the count is what is used.
DRAWN_COVERAGE = 0.5

#: A pixel has to differ by more than this to count as changed (out of 255).
CHANGED_LEVEL = 8


def set_desktop(display, root, number: int) -> None:
    """The same request `wmctrl -s N` sends."""
    from Xlib.protocol import event as xevent

    message = xevent.ClientMessage(
        window=int(root.id),
        client_type=display.intern_atom("_NET_CURRENT_DESKTOP"),
        data=(32, [int(number), 0, 0, 0, 0]),
    )
    root.send_event(message, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    display.flush()


def net_wm_desktop(display, xid: int) -> int | None:
    """The ``_NET_WM_DESKTOP`` of a window, or None if it will not say."""
    try:
        window = display.create_resource_object("window", xid)
        prop = window.get_full_property(display.intern_atom("_NET_WM_DESKTOP"), X.AnyPropertyType)
    except (XError, ValueError, AttributeError):
        return None
    if prop is None or not len(prop.value):
        return None
    return int(prop.value[0])


def window_state(display, xid: int) -> str:
    """What the X server and the manager currently think of the overlay.

    ``map_state`` is the one that says whether the overlay is actually on
    screen at all; ``states`` is what the window manager wrote back (a sticky
    window should carry ``_NET_WM_STATE_STICKY``); ``desktop`` is the property
    we asked for, echoed back.
    """
    try:
        window = display.create_resource_object("window", xid)
        attributes = window.get_attributes()
        state = window.get_full_property(display.intern_atom("_NET_WM_STATE"), X.AnyPropertyType)
        desktop = window.get_full_property(display.intern_atom("_NET_WM_DESKTOP"), X.AnyPropertyType)
    except (XError, AttributeError, ValueError):
        return "unreadable"
    names = [display.get_atom_name(int(atom)) for atom in (state.value if state else [])]
    map_state = {0: "unmapped", 1: "unviewable", 2: "viewable"}.get(
        int(getattr(attributes, "map_state", -1)), "?"
    )
    shown = "unset" if desktop is None or not len(desktop.value) else int(desktop.value[0])
    return f"map_state={map_state} desktop={shown} states=[{' '.join(names)}]"


def fake_entries(count: int = 3) -> list[AppEntry]:
    """Cards nobody has pixels for: enough to make the overlay paint."""
    return [
        AppEntry(
            key=f"probe{index}",
            label=f"Probe {index}",
            windows=[WindowRef(0x1000000 + index, f"probe {index}",
                               (40 + 30 * index, 40 + 30 * index, 700, 500))],
        )
        for index in range(count)
    ]


def pump(seconds: float) -> None:
    """Run the GTK loop for a while, so the overlay gets to draw."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        while Gtk.events_pending():
            Gtk.main_iteration_do(False)
        time.sleep(0.01)


def changed_fraction(a, b, threshold: int = CHANGED_LEVEL) -> float:
    """Fraction of pixels that differ by more than ``threshold`` levels."""
    from PIL import ImageChops

    difference = ImageChops.difference(a.convert("RGB"), b.convert("RGB")).convert("L")
    histogram = difference.histogram()
    total = sum(histogram)
    if not total:
        return 0.0
    return sum(histogram[threshold + 1:]) / total


def strip_difference(manager, carousel, entries) -> tuple[float | None, float | None]:
    """How the screen changes when the strip opens: (mean, changed fraction)."""
    root = manager.display.screen().root
    without = capture.grab_screen(manager.display, root)
    carousel.open(entries, 0)
    pump(0.4)
    with_strip = capture.grab_screen(manager.display, root)
    carousel.abort()
    pump(0.3)
    if without is None or with_strip is None:
        return None, None
    return capture.mean_difference(with_strip, without), changed_fraction(with_strip, without)


def verdict(difference: float | None, changed: float | None) -> str:
    if difference is None or changed is None:
        return "UNKNOWN (grab failed)"
    if changed >= DRAWN_COVERAGE:
        return "drawn"
    if changed <= 0.05:
        return "NOT DRAWN -- the compositor is not showing the overlay here"
    return f"only partly drawn ({changed:.0%} of the screen changed)"


def report(difference: float | None, changed: float | None) -> str:
    if difference is None or changed is None:
        return "(grab failed)"
    return f"mean {difference:5.1f}, {changed:4.0%} of pixels"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--switch", action="store_true",
                        help="measure on the next workspace too, then come back")
    args = parser.parse_args(argv)

    manager = WindowManager(config_module.load())
    # Built exactly the way the app builds it: the sticky request goes out while
    # the window is realized but not yet mapped, which is the only moment a
    # window manager is listening for it.
    carousel = Carousel(config_module.Config(), make_sticky=manager.make_sticky)
    carousel.ensure_window()
    gdk_window = carousel.window.get_window()
    if gdk_window is None:
        print("the overlay window was not created; is DISPLAY set?")
        return 1
    xid = int(gdk_window.get_xid())

    # Let the map land and the sticky retries go out: the property alone is not
    # the answer (Muffin leaves whatever we wrote there), `_NET_WM_STATE` is.
    pump(1.5)
    desktop = net_wm_desktop(manager.display, xid)
    print(f"overlay 0x{xid:x}  _NET_WM_DESKTOP = {desktop}")

    here = manager.current_desktop()
    entries = fake_entries()
    print(f"  {window_state(manager.display, xid)}")
    difference, changed = strip_difference(manager, carousel, entries)
    print(f"desktop {here}: opening the strip: {report(difference, changed)} -- "
          f"{verdict(difference, changed)}")

    if args.switch:
        count = manager.desktop_count()
        if count < 2:
            print("only one workspace: nothing to switch to")
        else:
            target = (here + 1) % count
            root = manager.display.screen().root
            print(f"--- switching {here} -> {target} (and waiting for it to settle) ---")
            set_desktop(manager.display, root, target)
            time.sleep(3.0)
            print(f"_NET_CURRENT_DESKTOP = {manager.current_desktop()}")
            print(f"  {window_state(manager.display, xid)}")
            difference, changed = strip_difference(manager, carousel, entries)
            print(f"desktop {target}: opening the strip: {report(difference, changed)} -- "
                  f"{verdict(difference, changed)}")
            set_desktop(manager.display, root, here)
            time.sleep(0.5)

    carousel.close()
    manager.display.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
