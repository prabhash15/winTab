#!/usr/bin/env python3
"""Is every card cropped from the right place -- and does switching work?

Two questions the strip cannot answer for itself, because its own tests compare
its output against its own idea of where the windows are:

* **Where is this window on screen?** A card is the screen cropped at the
  window's root-relative rect. X gives a window its position *inside its
  parent* (``get_geometry``) unless you translate it to the root, and that
  translation is easy to get wrong in both direction and field name -- which
  produces cards showing a random slice of the screen, not the window. This
  tool prints the app's rect next to the window's true position, and compares
  the crop against the window's own storage, so "the previews look wrong" is
  answered with a number instead of a guess.

* **Does switching to a window work?** ``--activate`` walks the windows the
  strip would show, asks the window manager to activate each one with exactly
  the request the app sends, and reports whether focus followed.

    python3 tools/probe_windows.py                 # where is everything?
    python3 tools/probe_windows.py --workspace     # only this workspace
    python3 tools/probe_windows.py --activate      # + try switching to each
"""

from __future__ import annotations

import argparse
import pathlib
import select
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from Xlib import X, display as xdisplay  # noqa: E402
from Xlib.error import XError  # noqa: E402
from Xlib.ext import composite  # noqa: E402,F401  (registers the extension)

from wintab import capture, config as config_module  # noqa: E402
from wintab.apps import WindowManager, usable_windows  # noqa: E402


class Reader:
    """A window's own pixels: name its pixmap, read it, free it."""

    def __init__(self, display):
        self.display = display
        self.errors: list[str] = []
        display.set_error_handler(self._on_error)

    def _on_error(self, error, request=None) -> None:
        self.errors.append(f"{type(error).__name__}")

    def frame_pixels(self, frame_id: int, width: int, height: int):
        """Image, or a string saying what went wrong."""
        del self.errors[:]
        frame = self.display.create_resource_object("window", frame_id)
        try:
            pixmap = frame.composite_name_window_pixmap()
            image = pixmap.get_image(0, 0, width, height, X.ZPixmap, 0xFFFFFFFF)
            pixmap.free()
            self.display.sync()
            picture = capture.image_from_ximage(bytes(image.data), width, height)
            if picture is None:
                return "undecodable pixels"
            if self.errors:
                return f"X error {self.errors[-1]} (still readable)"
            return picture
        except XError as exc:
            self.display.sync()
            reason = self.errors[-1] if self.errors else type(exc).__name__
            return f"X error {reason}"


def _signed(value) -> int:
    return int(value) & 0xFFFF


def server_timestamp(display) -> int:
    """A real X server timestamp, harvested from a throwaway window.

    Used to imitate what the app does today: it remembers the time of the
    last property change it saw and reuses it much later, which is exactly the
    kind of old timestamp focus-stealing prevention exists to reject.
    """
    from Xlib import Xatom

    root = display.screen().root
    scratch = root.create_window(
        0, 0, 1, 1, 0, display.screen().root_depth,
        X.InputOutput, X.CopyFromParent,
        event_mask=X.PropertyChangeMask, override_redirect=True,
    )
    try:
        # The property needs a real type atom: a type of None deletes it.
        scratch.change_property(
            display.intern_atom("WINTAB_PROBE"), Xatom.STRING, 8, b"1"
        )
        display.flush()
        ready, _, _ = select.select([display.fileno()], [], [], 2.0)
        while ready and display.pending_events():
            event = display.next_event()
            if event.type == X.PropertyNotify:
                return int(event.time)
    finally:
        scratch.destroy()
        display.flush()
        while display.pending_events():
            display.next_event()
    return 0


def activate_with(display, root, xid: int, timestamp: int) -> None:
    """The same ClientMessage the app sends, with a chosen timestamp."""
    from Xlib.protocol import event as xevent

    window = display.create_resource_object("window", xid)
    message = xevent.ClientMessage(
        window=window,
        client_type=display.intern_atom("_NET_ACTIVE_WINDOW"),
        data=(32, [2, timestamp, 0, 0, 0]),
    )
    root.send_event(message, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    display.flush()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workspace", action="store_true",
                        help="only windows the strip would show")
    parser.add_argument("--activate", action="store_true",
                        help="try switching to each window and report what happened")
    args = parser.parse_args(argv)

    manager = WindowManager(config_module.load())
    displaya = manager.display
    reader = Reader(displaya)
    root = displaya.screen().root

    windows = manager.list_windows()
    current = manager.current_desktop()
    if args.workspace:
        windows = usable_windows(windows, current, manager._own_pid)
    if not windows:
        print("no windows to look at")
        return 0

    snapshot = capture.grab_screen(displaya, root)
    print(f"screen {snapshot.size[0]}x{snapshot.size[1]}, {len(windows)} window(s)")

    for win in windows:
        handle = displaya.create_resource_object("window", win.xid)
        try:
            attributes = handle.get_attributes()
            geometry = handle.get_geometry()
            reply = root.translate_coords(handle, 0, 0)
            truth = (int(reply.x) & 0xFFFF, int(reply.y) & 0xFFFF)
        except XError as exc:
            print(f"0x{win.xid:x} vanished: {exc}")
            continue
        viewable = attributes is not None and attributes.map_state == X.IsViewable
        app_rect = win.rect
        offset = (truth[0] - app_rect[0], truth[1] - app_rect[1])
        print(
            f"\n0x{win.xid:x} {win.title[:42]!r}\n"
            f"    frame=0x{win.frame:x} viewable={viewable} placed={win.placed} "
            f"client {geometry.width}x{geometry.height} (parent-relative "
            f"{_signed(geometry.x)},{_signed(geometry.y)})\n"
            f"    true top-left {truth}  app rect {app_rect}  "
            f"{'MATCHES' if offset == (0, 0) else f'OFF BY {offset}'}"
        )

        frame = displaya.create_resource_object("window", win.frame)
        frame_geometry = frame.get_geometry()
        if frame_geometry is None:
            print("    frame unreadable")
            continue
        fw, fh = frame_geometry.width, frame_geometry.height
        own = reader.frame_pixels(win.frame, fw, fh)
        if isinstance(own, str):
            print(f"    frame is {fw}x{fh}; own pixels: {own}")
            if win.width and (fw, fh) != (win.width, win.height):
                print(f"    (app thinks the window is {win.width}x{win.height})")
            continue

        print(f"    frame is {fw}x{fh}, app thinks {win.width}x{win.height}")
        left, top = app_rect[0], app_rect[1]
        box = (left, top, left + fw, top + fh)
        on_screen = box[0] >= 0 and box[1] >= 0 and box[2] <= snapshot.size[0] and box[3] <= snapshot.size[1]
        if not on_screen:
            print(f"    crop at {box} would come out partly empty (off the screen)")
            continue
        difference = capture.mean_difference(snapshot.crop(box), own)
        print(
            f"    crop at {box} vs the window's own pixels: mean diff {difference:6.1f}"
            f"{'   <-- occluded or misplaced' if difference > 12 else '   (same)'}"
        )

    if args.activate:
        print("\nswitching with the app's own request (data[1] = CurrentTime today):")
        original = manager.active_window()
        for win in windows:
            manager.activate(win.xid)
            time.sleep(0.35)
            active = manager.active_window()
            verdict = "OK" if active == win.xid else f"NOT FOCUSED (active=0x{active or 0:x})"
            print(f"    0x{win.xid:x} {win.title[:30]!r}: {verdict}")

        # The app used to send the time of the last property change it saw,
        # which can be minutes old by the time you swipe. Does the window
        # manager still obey that?
        print("\nswitching with an old timestamp (what the app sent before):")
        stamp = server_timestamp(displaya)
        print(f"    (timestamp {stamp}, used 3s later)")
        time.sleep(3.0)
        for win in windows:
            activate_with(displaya, root, win.xid, stamp)
            time.sleep(0.35)
            active = manager.active_window()
            verdict = "OK" if active == win.xid else f"IGNORED (active=0x{active or 0:x})"
            print(f"    0x{win.xid:x} {win.title[:30]!r}: {verdict} [t={stamp}, 3s old]")

        if original:
            manager.activate(original)
            time.sleep(0.35)
            print(f"    restored 0x{original:x}: "
                  f"{'OK' if manager.active_window() == original else 'FAILED'}")

    displaya.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
