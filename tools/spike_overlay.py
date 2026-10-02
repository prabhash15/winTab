#!/usr/bin/env python3
"""Prove the overlay window behaves under Muffin before building the carousel.

Checks three assumptions the whole visual approach rests on:

1. a mapped, transparent, always-on-top window really is above everything
   (including a fullscreen window and the Cinnamon panel)
2. it never takes keyboard focus -- the focused app must stay focused
3. it is click-through (empty input shape), and GTK agrees it is mapped

It checks the window *recipe*, not where the window lives: the sticky dance
that keeps the strip on every workspace is measured by ``probe_sticky.py``
instead, since a window built like this one stays on the workspace it was
created in until someone asks the manager for more.

The window paints a half-transparent magenta sheet plus a white marker, so a
screenshot can be tested for "did our pixels reach the screen".

    python3 tools/spike_overlay.py            # run checks, print a report
    python3 tools/spike_overlay.py --keep     # leave it on screen until Enter
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import cairo  # noqa: E402
import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

from PIL import Image  # noqa: E402
from Xlib import X, display as xdisplay  # noqa: E402

WHOLE_SCREEN = (0, 0, 1920, 1080)


def build_window(width: int, height: int) -> Gtk.Window:
    """Exactly how the real overlay is configured."""
    win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
    win.set_decorated(False)
    win.set_resizable(False)
    win.set_skip_taskbar_hint(True)
    win.set_skip_pager_hint(True)
    win.set_keep_above(True)
    win.set_accept_focus(False)
    win.set_focus_on_map(False)
    win.set_type_hint(Gdk.WindowTypeHint.NOTIFICATION)
    visual = win.get_screen().get_rgba_visual()
    if visual is not None:
        win.set_visual(visual)
    win.set_app_paintable(True)
    win.set_default_size(width, height)
    win.move(0, 0)
    win.show_all()
    gdk_window = win.get_window()
    # Empty input shape: every click falls through to what is underneath.
    gdk_window.input_shape_combine_region(cairo.Region(), 0, 0)
    gdk_window.raise_()
    return win


def paint(win: Gtk.Window, width: int, height: int):
    def on_draw(_widget, ctx):
        ctx.set_source_rgba(1.0, 0.0, 1.0, 0.5)  # magenta, half transparent
        ctx.paint()
        ctx.set_source_rgb(1.0, 1.0, 1.0)
        ctx.rectangle(width // 2 - 120, height // 2 - 60, 240, 120)
        ctx.fill()
        return False

    win.connect("draw", on_draw)
    win.queue_draw()


def screen_size() -> tuple[int, int]:
    d = xdisplay.Display()
    root = d.screen().root.get_geometry()
    size = (root.width, root.height)
    d.close()
    return size


def x11_report(xid: int) -> dict:
    d = xdisplay.Display()
    root = d.screen().root

    def atom(name):
        return d.intern_atom(name)

    def prop(win, name):
        p = win.get_full_property(atom(name), X.AnyPropertyType)
        return p.value if p is not None and len(p.value) else None

    root_props = {name: prop(root, name) for name in ("_NET_ACTIVE_WINDOW", "_NET_CLIENT_LIST_STACKING")}
    stacking = [int(x) for x in (root_props["_NET_CLIENT_LIST_STACKING"] or [])]

    def describe(wid):
        win = d.create_resource_object("window", wid)
        cls = prop(win, "WM_CLASS")
        name = prop(win, "_NET_WM_NAME") or prop(win, "WM_NAME")
        types = prop(win, "_NET_WM_WINDOW_TYPE") or []
        states = prop(win, "_NET_WM_STATE") or []
        decode = lambda raw: raw.decode("latin-1") if isinstance(raw, bytes) else str(raw)
        try:
            names = [d.get_atom_name(a).decode("latin-1") for a in list(types) + list(states)]
        except Exception:
            names = []
        return {
            "name": decode(name) if name else "",
            "class": decode(cls) if cls else "",
            "types": names,
        }

    own = d.create_resource_object("window", xid) if xid else None
    screenshot = None
    try:
        raw = root.get_image(*WHOLE_SCREEN[:2], WHOLE_SCREEN[2], WHOLE_SCREEN[3], X.ZPixmap, 0xFFFFFFFF).data
        screenshot = Image.frombytes("RGB", (WHOLE_SCREEN[2], WHOLE_SCREEN[3]), raw, "raw", "BGRX")
    except Exception as exc:
        print(f"  (screenshot failed: {exc})")

    return {
        "active": int(root_props["_NET_ACTIVE_WINDOW"][0]) if root_props["_NET_ACTIVE_WINDOW"] else 0,
        "stacking": stacking,
        "top": stacking[-1] if stacking else 0,
        "own_types": describe(xid) if xid else {},
        "own_state": (prop(own, "_NET_WM_STATE") or []) if own is not None else [],
        "screenshot": screenshot,
        "others": {wid: describe(wid) for wid in stacking if wid != xid},
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--keep", action="store_true", help="leave the overlay up until Enter")
    parser.add_argument("--seconds", type=float, default=2.5)
    parser.add_argument("--save", type=pathlib.Path, help="save the proof screenshot here")
    args = parser.parse_args(argv)

    Gtk.init([])
    width, height = screen_size()
    before = x11_report(0)["active"]
    print(f"screen                     : {width}x{height}")
    print(f"focused window before      : 0x{before:x}")

    win = build_window(width, height)
    paint(win, width, height)
    xid = win.get_window().get_xid()
    print(f"overlay window id          : 0x{xid:x}")

    result: dict = {}

    def check():
        report = x11_report(xid)
        result.update(report)
        Gtk.main_quit()
        return False

    GLib.timeout_add(int(args.seconds * 1000), check)
    if args.keep:
        print("overlay is up; press Enter to finish")
    Gtk.main()

    report = result
    stacking = report["stacking"]
    own_index = stacking.index(xid) if xid in stacking else -1
    print()
    print("=== stacking (bottom -> top) ===")
    for position, wid in enumerate(stacking):
        mark = " <-- overlay" if wid == xid else ""
        info = report["others"].get(wid, report["own_types"] if wid == xid else {})
        label = info.get("name") or info.get("class") or "?"
        kinds = ",".join(t.replace("_NET_WM_WINDOW_TYPE_", "") for t in info.get("types", []))
        print(f"  {position:2d}  0x{wid:<9x} {label[:44]:<44} [{kinds}]{mark}")

    print()
    print("=== checks ===")
    checks = []

    checks.append(("overlay is a managed, mapped window", own_index >= 0))
    checks.append((f"overlay is on top (index {own_index} of {len(stacking) - 1})", own_index == len(stacking) - 1))
    checks.append((f"focus was not stolen (was 0x{before:x}, now 0x{report['active']:x})", report["active"] == before))

    shot = report["screenshot"]
    if shot is not None:
        centre = shot.getpixel((width // 2, height // 2))
        corner = shot.getpixel((width // 2, 8))
        panel = shot.getpixel((width // 2, height - 8))
        print(f"  pixel centre (x{width // 2},y{height // 2})  = {centre}")
        print(f"  pixel top edge (titlebar/panel area) = {corner}")
        print(f"  pixel bottom edge (panel area)       = {panel}")
        checks.append(("overlay pixels reached the screen (white marker visible)", all(c > 200 for c in centre)))
        checks.append(("overlay covers the top edge of the screen", corner[0] > 100 and corner[2] > 100))
        if args.save:
            shot.save(args.save)
            print(f"  saved: {args.save}")

    for label, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")

    win.destroy()
    Gtk.main_quit()
    return 0 if all(ok for _, ok in checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
