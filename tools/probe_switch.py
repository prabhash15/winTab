#!/usr/bin/env python3
"""After a workspace switch, does the strip see the *new* workspace's pixels?

Every card comes from one of two places, and each has a way of being wrong the
moment the workspace changes:

* the window's **own pixels**, read from the compositor -- valid on any
  workspace, but only if the window is still redirected
* a **crop of the last screen grab** -- only valid if that grab was taken on the
  workspace the rects belong to

The app trusts the grab it asked for, and stamps it with the workspace it *wanted*,
not the one the screen was actually showing. A grab taken during Cinnamon's
workspace-switch animation is a blend of both workspaces, and if nothing on
screen changes afterwards nothing ever replaces it: every cropped card then
shows the previous workspace for the rest of the session. That is the "the
applications are not showing their windows" report, so this tool measures it
instead of guessing.

    python3 tools/probe_switch.py            # read-only: what is on each desktop
    python3 tools/probe_switch.py --switch   # switch away, sample, switch back
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from Xlib import X  # noqa: E402
from Xlib.error import XError  # noqa: E402
from Xlib.ext import composite  # noqa: E402,F401  (registers the extension)

from wintab import capture, config as config_module  # noqa: E402
from wintab.apps import WindowManager, usable_windows, windows_needing_own_pixels  # noqa: E402


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


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--switch", action="store_true",
                        help="switch workspace away and back, sampling as we go")
    parser.add_argument("--hold", type=float, default=1.6,
                        help="how long to stay on the other workspace (seconds)")
    parser.add_argument("--to", type=int, default=None,
                        help="which workspace to switch to (default: the next one)")
    args = parser.parse_args(argv)

    manager = WindowManager(config_module.load())
    display = manager.display
    root = display.screen().root

    everything = manager.list_windows()
    current = manager.current_desktop()
    count = manager.desktop_count()
    print(f"desktop {current} of {count}; {len(everything)} window(s) in _NET_CLIENT_LIST")

    for desktop in range(count):
        here = [win for win in everything if win.desktop == desktop]
        print(f"\n--- desktop {desktop} ({len(here)} window(s)) ---")
        if not here:
            print("    (empty)")
            continue
        for win in here:
            own = capture.read_window_pixels(display, win.frame) if win.frame else None
            pixels = f"{own.width}x{own.height}" if own is not None else "UNAVAILABLE"
            print(
                f"    0x{win.xid:x} {win.title[:38]!r}\n"
                f"        frame=0x{win.frame:x} rect={win.rect} placed={win.placed} "
                f"minimized={win.minimized} own pixels: {pixels}"
            )

    if not args.switch:
        display.close()
        return 0

    target = current + 1 if args.to is None else args.to
    target %= count
    if count < 2:
        print("\nonly one workspace: nothing to switch to")
        display.close()
        return 0

    baseline = capture.grab_screen(display, root)
    print(f"\n=== switching {current} -> {target} and sampling ===")
    set_desktop(display, root, target)
    started = time.monotonic()
    previous = None
    samples = (0.15, 0.35, 0.75, 1.5)
    for mark in samples:
        time.sleep(max(0.0, started + mark - time.monotonic()))
        elapsed = time.monotonic() - started
        reported = manager.current_desktop()
        plain = capture.grab_screen(display, root)
        if plain is None:
            print(f"  t={elapsed:4.2f}s _NET_CURRENT_DESKTOP={reported}  grab FAILED")
            continue
        diff = "n/a" if previous is None else f"{capture.mean_difference(plain, previous):5.1f}"
        here = usable_windows(manager.list_windows(), reported, manager._own_pid)
        occluded = windows_needing_own_pixels(
            manager.list_windows(), reported, manager.screen_geometry(), manager._own_pid
        )
        readable = 0
        worst = 0.0
        for win in here:
            own = capture.read_window_pixels(display, win.frame) if win.frame else None
            if own is not None:
                readable += 1
                box = (win.rect[0], win.rect[1], win.rect[0] + own.width, win.rect[1] + own.height)
                left, top = max(0, box[0]), max(0, box[1])
                right = min(plain.size[0], box[2])
                bottom = min(plain.size[1], box[3])
                if right - left > 8 and bottom - top > 8:
                    crop = plain.crop((left, top, right, bottom))
                    worst = max(worst, capture.mean_difference(crop, own))
        print(
            f"  t={elapsed:4.2f}s _NET_CURRENT_DESKTOP={reported}  "
            f"grab vs previous: {diff:>6}  windows here: {len(here)}  "
            f"own pixels readable: {readable}  occluded: {len(occluded)}  "
            f"worst crop-vs-own diff: {worst:5.1f}"
        )
        previous = plain

    print(f"=== switching back to {current} ===")
    set_desktop(display, root, current)
    time.sleep(0.6)
    print(f"  _NET_CURRENT_DESKTOP={manager.current_desktop()}")
    settled = capture.grab_screen(display, root)
    if settled is not None and baseline is not None:
        print(f"  settled grab vs the grab before the switch: "
              f"{capture.mean_difference(settled, baseline):5.1f}")
    display.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
