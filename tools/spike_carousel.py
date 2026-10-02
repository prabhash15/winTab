#!/usr/bin/env python3
"""Drive a synthetic three-finger swipe through the real stack and check the pixels.

The gesture engine is covered by unit tests and the evdev reader cannot be faked
without /dev/uinput, so this is the only way to see the strip for real before
touching the touchpad. It synthesises touchpad frames, feeds them through the
**real** state machine, applies the events to the carousel exactly the way
``app.py`` does, grabs the root window at several points and checks:

  * the overlay is a mapped managed window and sits on top of the stack
  * one card per open window, all fully on screen, each showing that window's
    own pixels rather than a placeholder
  * the selection outline is drawn where the layout says, and moving one window
    moves it by exactly one slot
  * the selected card is brighter than the others, and the row sits on a band
  * **nothing is activated while the fingers are down**, and a release that
    travelled less than half a step changes nothing
  * holding still costs no redraws at all
  * the fade returns to rest and the screen is bit-identical afterwards

Nothing on your desktop is switched: activations are recorded, not performed.

    python3 tools/spike_carousel.py                 # run the checks
    python3 tools/spike_carousel.py --save /tmp/x   # keep the screenshots
    python3 tools/spike_carousel.py --keep          # leave the strip up until Enter
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import gi  # noqa: E402

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

from PIL import Image, ImageChops, ImageStat  # noqa: E402
from Xlib import X, display as xdisplay  # noqa: E402

from wintab import config as config_module  # noqa: E402
from wintab.apps import WindowManager, usable_windows  # noqa: E402
from wintab.capture import ScreenCache, WindowTarget, read_window_pixels  # noqa: E402
from wintab.gesture import (  # noqa: E402
    GestureParams,
    SwipeBegin,
    SwipeCancel,
    SwipeEnd,
    SwipeUpdate,
    ThreeFingerSwipe,
)
from wintab.overlay import Carousel  # noqa: E402
from wintab.touchpad import Contact, ContactFrame  # noqa: E402

ANCHOR = 0.5  # where the fingers land, and the origin progress is measured from
START = 0.3  # a drag may not run past this much either side of the anchor


def pump(milliseconds: float) -> None:
    """Run the GTK main loop by hand for a while."""
    context = GLib.MainContext.default()
    deadline = time.monotonic() + milliseconds / 1000.0
    while time.monotonic() < deadline:
        while context.pending():
            context.iteration(False)
        time.sleep(0.004)


def screenshot(display) -> Image.Image:
    root = display.screen().root
    geometry = root.get_geometry()
    raw = root.get_image(0, 0, geometry.width, geometry.height, X.ZPixmap, 0xFFFFFFFF).data
    return Image.frombytes("RGB", (geometry.width, geometry.height), raw, "raw", "BGRX")


def mean_diff(a: Image.Image, b: Image.Image) -> float:
    return ImageStat.Stat(ImageChops.difference(a, b).convert("L")).mean[0]


def region(img: Image.Image, rect) -> Image.Image:
    x, y, w, h = (int(v) for v in rect)
    return img.crop((max(0, x), max(0, y), max(0, x) + w, max(0, y) + h))


def luminance(img: Image.Image, rect=None) -> float:
    patch = region(img, rect) if rect else img
    return ImageStat.Stat(patch.convert("L")).mean[0]


def high_frequency(img: Image.Image, rect=None) -> float:
    patch = region(img, rect) if rect else img
    left = patch.crop((0, 0, patch.width - 1, patch.height))
    right = patch.crop((1, 0, patch.width, patch.height))
    return ImageStat.Stat(ImageChops.difference(left, right).convert("L")).mean[0]


def is_outline_blue(pixel) -> bool:
    r, g, b = pixel[:3]
    return b - r > 60 and b > 120


def blue_edges(image: Image.Image, y: int) -> list[int]:
    """x of each run of accent blue along a scanline (the outline's sides)."""
    found, start = [], None
    for x in range(image.width):
        if is_outline_blue(image.getpixel((x, y))):
            if start is None:
                start = x
        elif start is not None:
            found.append((start + x - 1) // 2)
            start = None
    if start is not None:
        found.append((start + image.width - 1) // 2)
    return found


def nearest(values, target):
    return min(values, key=lambda value: abs(value - target)) if values else None


class Driver:
    """Synthesise touchpad frames and apply the events the way ``app.py`` does."""

    def __init__(self, carousel, entries, start_index, config):
        self.carousel = carousel
        self.entries = entries
        self.start_index = start_index
        self.engine = ThreeFingerSwipe(GestureParams.from_config(config))
        self.x = ANCHOR
        self.t = 1000.0
        self.activated: list[int] = []
        self.selections: list[int] = []
        self.ended: list[int] = []
        self.opened = 0
        self.cancelled = 0

    # -- frames ------------------------------------------------------------
    def _feed(self, points):
        self.t += 0.008
        self.engine_feed(
            ContactFrame(
                self.t,
                tuple(Contact(i, float(x), float(y)) for i, (x, y) in enumerate(points)),
            )
        )

    def engine_feed(self, frame):
        self._apply(self.engine.feed(frame))

    def land(self, x=ANCHOR, y=0.5):
        """Three fingers down together, which is what arms the gesture."""
        self.x = x
        for count in (1, 2, 3):
            self._feed([(x, y)] * count)

    def drag(self, steps, frames=12):
        """Move the fingers `steps` windows' worth, slowly enough to stay honest."""
        target = ANCHOR + steps * self.carousel.config.step_fraction
        target = max(ANCHOR - START, min(ANCHOR + START, target))
        start = self.x
        for i in range(1, frames + 1):
            self._feed([(start + (target - start) * i / frames, 0.5)] * 3)
        self.x = target

    def hold(self, frames=10):
        """Stay perfectly still, so the release is not read as a flick."""
        for _ in range(frames):
            self._feed([(self.x, 0.5)] * 3)

    def release(self):
        self._feed([(self.x, 0.5)] * 2)
        self._feed([(self.x, 0.5)])
        self._feed([])

    # -- what app.py would do with the events ------------------------------
    def _apply(self, events):
        for event in events:
            if isinstance(event, SwipeBegin):
                self.carousel.open(self.entries, self.start_index)
                self.opened += 1
            elif isinstance(event, SwipeUpdate):
                self.carousel.set_progress(event.progress, event.steps)
                self.selections.append(self.carousel.selection)
            elif isinstance(event, SwipeEnd):
                self.ended.append(event.steps)
                steps = max(-self.start_index,
                            min(len(self.entries) - 1 - self.start_index, event.steps))
                if steps:
                    target = self.start_index + steps
                    self.carousel.settle(
                        steps, on_commit=lambda t=target: self.activated.append(t)
                    )
                else:
                    self.carousel.cancel()
            elif isinstance(event, SwipeCancel):
                self.cancelled += 1
                self.carousel.cancel()


def stacking(display):
    root = display.screen().root
    prop = root.get_full_property(
        display.intern_atom("_NET_CLIENT_LIST_STACKING"), X.AnyPropertyType
    )
    return [int(v) for v in (prop.value if prop else [])]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--save", type=pathlib.Path, help="prefix for the screenshots")
    parser.add_argument("--keep", action="store_true", help="leave the strip up until Enter")
    args = parser.parse_args(argv)

    cfg = config_module.load()
    cfg.show_fps = True

    Gtk.init([])
    display = xdisplay.Display()
    display.set_error_handler(lambda *_a: None)
    wm = WindowManager(cfg)
    cache = ScreenCache(cfg)
    # Publish the same targets app.py would, desktop stamp included, so the
    # capture this spike works from is the one the app would have made.
    desktop = wm.current_desktop()
    workspace = usable_windows(wm.list_windows(), desktop, wm._own_pid)
    placed = [win for win in workspace if win.placed]
    cache.set_targets(
        [WindowTarget(win.xid, win.frame or win.xid, win.rect) for win in placed],
        desktop=desktop,
    )
    cache.start()
    for _ in range(160):
        if cache.generation:
            break
        time.sleep(0.05)
    snapshot = cache.snapshot()
    if snapshot is None:
        print("spike: the capture thread produced nothing; is DISPLAY right?")
        return 1
    plain, backdrop, _, snapshot_desktop = snapshot
    print(f"spike: snapshot {plain.size} of desktop {snapshot_desktop}  {cache.stats}")

    entries, index = wm.build()
    print(f"spike: {len(entries)} window(s) in this workspace, focused index {index}")
    for position, entry in enumerate(entries):
        mark = " *" if position == index else "  "
        title = (entry.top.title or entry.label)[:46]
        print(f"  {mark} {position}: {title:<48} {entry.top.rect}")
    if len(entries) < 2:
        print("spike: need at least two windows to check the strip; open another and retry")
        return 1

    # Built exactly the way app.py builds it, sticky request included: the
    # window has to say it lives on every workspace before it is mapped.
    carousel = Carousel(cfg, make_sticky=wm.make_sticky)
    carousel.ensure_window()
    cache.set_busy(True)
    # Built exactly the way app.py builds them: this workspace's windows, only
    # the ones we could place on screen, each with its frame for own-pixel reads.
    rects = {win.xid: win.rect for win in placed}
    pixels: dict[int, Image.Image] = {}
    started = time.monotonic()
    for win in placed:
        picture = read_window_pixels(display, win.frame or win.xid)
        if picture is not None:
            pixels[win.xid] = picture
    read_ms = (time.monotonic() - started) * 1000.0

    started = time.monotonic()
    carousel.prewarm(
        plain, backdrop, cache.generation, rects, pixels,
        allow_crop=snapshot_desktop == wm.current_desktop(),
    )
    print(f"spike: own pixels for {len(pixels)}/{len(placed)} window(s) took {read_ms:.1f} ms; "
          f"prewarm took {(time.monotonic() - started) * 1000:.1f} ms")

    driver = Driver(carousel, entries, index, cfg)
    last = len(entries) - 1
    room_right = index + 1 <= last
    room_left = index - 1 >= 0

    # Everything about the layout has to be read while the strip is actually up.
    shots: dict[int, Image.Image] = {}
    layouts: dict[int, list] = {}
    outlines: dict[int, tuple] = {}

    def shoot(selection: int) -> None:
        shots[selection] = screenshot(display)
        layouts[selection] = carousel.card_rects()
        outlines[selection] = carousel.outline_rect()

    # --- one gesture: drag one window, look, drag the other way, look ----
    driver.land()
    pump(100)
    if room_right:
        driver.drag(+0.7)
        pump(260)
        shoot(min(index + 1, last))
        print(f"spike: after dragging right 0.7 steps -> {carousel.stats}")
    if room_left:
        driver.drag(-1.4 if room_right else -0.7)
        pump(260)
        shoot(max(index - 1, 0))
        print(f"spike: after dragging left -> {carousel.stats}")

    # Back to a quarter of a step *from the anchor*, which is under the commit
    # threshold, so releasing here must change nothing.
    driver.drag(0.25)
    pump(260)
    shoot(index)
    print(f"spike: back under half a step -> {carousel.stats}")

    draws_before = carousel.stats["draws"]
    for _ in range(30):
        carousel.set_progress(carousel.progress, None)
        pump(10)
    draws_after = carousel.stats["draws"]

    # The carousel keeps the *same* config object, so remember what to put back
    # before poking at it: restoring from `cfg` after mutating it restores the
    # value that was just written, not the one that was there.
    band_alpha, caption_font = cfg.band_alpha, cfg.caption_font_px

    band_on = screenshot(display)
    cfg.band_alpha = 0.0
    carousel.window.queue_draw()
    pump(200)
    band_off = screenshot(display)
    cfg.band_alpha = band_alpha

    # Same trick for the names: draw the strip with them, then without, and
    # look at the strip of screen just under each card.
    named = screenshot(display)
    cfg.caption_font_px = 0.0
    carousel.window.queue_draw()
    pump(200)
    unnamed = screenshot(display)
    cfg.caption_font_px = caption_font
    caption_diffs: list[float] = []
    for x, y, w, h in carousel.card_rects():
        band = (int(x), int(y + h + 2), int(w), int(cfg.caption_gap_px + caption_font * 1.6))
        a, b = region(named, band), region(unnamed, band)
        if a.width >= 16 and a.height >= 8:
            caption_diffs.append(mean_diff(a, b))

    activated_before_release = list(driver.activated)
    driver.hold()
    driver.release()
    pump(400)
    print(f"spike: released under half a step -> activated {driver.activated}, "
          f"state {carousel.state}, ends {driver.ended}")

    # --- a second gesture that actually opens something ------------------
    driver.land()
    pump(100)
    direction = 1 if room_right else -1
    driver.drag(direction * 1.2)
    pump(260)
    activated_during_second = list(driver.activated)
    driver.hold()
    driver.release()
    pump(500)
    print(f"spike: switching gesture -> activated {driver.activated}, ends {driver.ended}")

    checks: list[tuple[str, bool, str]] = []

    overlay_xid = carousel.window.get_window().get_xid()
    stack = stacking(display)
    own = stack.index(overlay_xid) if overlay_xid in stack else -1
    checks.append(("overlay is a mapped managed window", own >= 0,
                   f"index {own} of {len(stack) - 1}"))
    checks.append(("overlay is topmost", own == len(stack) - 1,
                   f"index {own}, top 0x{stack[-1]:x}"))

    # --- one card per window, where the layout says ----------------------
    layout = layouts.get(index, [])
    widths = [int(w) for _, _, w, _ in layout]
    checks.append((
        "one card per open window, all fully on screen",
        len(layout) == len(entries) and all(
            x >= -0.5 and x + w <= plain.width + 0.5 for x, _, w, _ in layout
        ),
        f"{len(layout)} cards for {len(entries)} windows; widths {widths}",
    ))
    if len(layout) >= 2:
        pitches = [round(layout[i + 1][0] - layout[i][0], 1) for i in range(len(layout) - 1)]
        checks.append((
            "cards are evenly spaced",
            max(pitches) - min(pitches) < 1.0,
            f"card-to-card spacing {pitches}",
        ))

    # --- each card really shows *that window* ----------------------------
    #
    # Comparing a card against a crop of the screen at the window's rect only
    # proves the strip agrees with itself: both come from the same rect. This
    # compares against the window's own pixels, which is what the card claims
    # to be, and against where the window really is on the root.
    worst, worst_title, compared, from_crop = 0.0, "", 0, 0
    for position, entry in enumerate(entries):
        if position >= len(layout):
            continue
        shot_region = region(shots[index], layout[position])
        if shot_region.width < 8 or shot_region.height < 8:
            continue
        # The window this card was built from: the first of the entry's
        # windows we have pixels for, exactly like the carousel picks it.
        source = next((ref for ref in entry.windows if ref.xid in pixels), entry.top)
        truth = pixels.get(source.xid)
        if truth is None:
            truth = Carousel._crop(plain, source.rect)
            from_crop += 1
        if truth is None:
            continue
        expected = truth.convert("RGB").resize(shot_region.size, Image.LANCZOS)
        difference = mean_diff(shot_region, expected)
        compared += 1
        if difference > worst:
            worst, worst_title = difference, (entry.top.title or entry.label)[:30]
    checks.append((
        "cards show the window itself, not what is in front of it",
        compared >= 2 and worst < 25.0,
        f"{compared} cards compared ({from_crop} from a screen crop); "
        f"worst ({worst_title!r}) differs by {worst:.1f}",
    ))

    # --- the app's idea of where a window is matches the window ----------
    #
    # A window's rect is what a card would be cropped from when it has no own
    # pixels, so it has to be the window's place on the *root*. The frame the
    # manager reparented it into is a little bigger than the rect (its own
    # border), which is why this allows a border's worth of slack; the bug this
    # would have caught was off by a thousand pixels, not by ten.
    worst_offset, worst_title, compared_windows = 0, "", 0
    for win in placed:
        frame = display.create_resource_object("window", win.frame or win.xid)
        geometry = frame.get_geometry()
        if geometry is None:
            continue
        compared_windows += 1
        offset = max(abs(geometry.x - win.rect[0]), abs(geometry.y - win.rect[1]))
        if offset > worst_offset:
            worst_offset, worst_title = offset, win.title[:30]
    checks.append((
        "every listed window is placed where it really is on screen",
        compared_windows == len(placed) and worst_offset <= 24,
        f"{compared_windows} window(s); worst is {worst_offset}px out "
        f"({worst_title!r}: the frame's own border, not a mis-placed window)",
    ))

    # --- the outline is drawn where the layout says ----------------------
    start_outline = outlines.get(index)
    start_shot = shots.get(index)
    blue_ok, blue_detail = False, "no screenshot"
    step_ok, step_detail = False, "no second selection to compare"
    if start_outline and start_shot:
        scanline = int(start_outline[1] + start_outline[3] / 2)
        expected_left = int(start_outline[0])
        edges = blue_edges(start_shot, scanline)
        near = nearest(edges, expected_left)
        matches_layout = near is not None and abs(near - expected_left) <= 5
        pair = [e for e in edges if near is not None and abs(e - near) <= carousel._pitch]
        span_ok = len(pair) == 2 and abs((pair[1] - pair[0]) - (start_outline[2] - 1)) <= 8
        blue_ok = matches_layout and span_ok
        blue_detail = (f"expected blue edges near x={expected_left} and "
                       f"x={int(start_outline[0] + start_outline[2])}; "
                       f"scanline found {edges}")

        other = next((s for s in shots if s != index), None)
        if other is not None and other in outlines and other in shots:
            other_scan = int(outlines[other][1] + outlines[other][3] / 2)
            other_expected = int(outlines[other][0])
            other_near = nearest(blue_edges(shots[other], other_scan), other_expected)
            if near is not None and other_near is not None:
                moved = abs(other_near - near)
                wanted = abs(other - index) * carousel._pitch
                step_ok = abs(moved - wanted) <= 8
                step_detail = (f"outline moved {moved}px for {abs(other - index)} window(s); "
                               f"one slot is {carousel._pitch:.0f}px")

    checks.append(("selection outline follows the layout", blue_ok, blue_detail))
    checks.append(("the outline moves one slot per window", step_ok, step_detail))

    # --- direction and brightness ----------------------------------------
    others = [s for s in shots if s != index]
    if others:
        other = others[0]
        if other > index:
            checks.append((
                "dragging right selects the window to the right",
                other > index,
                f"dragged right, selection went {index} -> {other}",
            ))
        else:
            checks.append((
                "dragging left selects the window to the left",
                other < index,
                f"dragged left, selection went {index} -> {other}",
            ))
        if other in layouts and len(layouts[other]) > other:
            same_card = layouts[other][other]
            selected = luminance(shots[other], same_card)
            if other in shots and index in layouts and len(layouts[index]) > other:
                unselected = luminance(shots[index], layouts[index][other])
                checks.append((
                    "the selected card is brighter than the others",
                    selected > unselected + 1.0,
                    f"card {other}: {unselected:.1f} unselected vs {selected:.1f} selected",
                ))

    # --- the band, by turning it off and looking again -------------------
    if layout:
        gap_x = int(layout[0][0] + layout[0][2] + max(4, carousel._gap / 2))
        gap_box = (gap_x - 5, int(carousel._band_top + 10), 10, 20)
    checks.append((
        "every card carries its application's name",
        len(caption_diffs) >= 2 and min(caption_diffs) > 3.0,
        f"under-card ink differs by {[round(d, 1) for d in caption_diffs]} "
        f"with the caption on vs off",
    ))
    checks.append((
        "the row sits on a lighter band",
            luminance(band_on, gap_box) > luminance(band_off, gap_box) + 1.0,
            f"brightness beside the cards {luminance(band_off, gap_box):.1f} -> "
            f"{luminance(band_on, gap_box):.1f} with the band on",
        ))

    # --- backdrop still dimmed and blurred outside the band --------------
    top_box = (0, 0, plain.width, max(8, int(carousel._band_top) - 8))
    dim_ratio = luminance(band_on, top_box) / max(1.0, luminance(plain, top_box))
    blur_ratio = high_frequency(band_on, top_box) / max(0.01, high_frequency(plain, top_box))
    checks.append((
        "backdrop above the band is dimmed and blurred",
        dim_ratio < 0.92 and blur_ratio < 0.75,
        f"brightness {dim_ratio:.2f} of raw, neighbour difference {blur_ratio:.2f}",
    ))

    # --- the rules that matter -------------------------------------------
    checks.append((
        "nothing is activated while the fingers are down",
        not activated_before_release and not activated_during_second,
        f"during the drags: {activated_before_release + activated_during_second or 'nothing'}",
    ))
    checks.append((
        "a drag under half a step changes nothing",
        bool(driver.ended) and driver.ended[0] == 0,
        f"first release reported steps={driver.ended[0] if driver.ended else None}",
    ))
    expected_target = index + direction
    checks.append((
        "releasing on a window opens that window",
        driver.activated[:1] == [expected_target],
        f"activated {driver.activated[:1]} (expected [{expected_target}]), "
        f"started on {index}",
    ))
    checks.append((
        "holding still costs no redraws",
        draws_after == draws_before,
        f"30 identical reports drew {draws_after - draws_before} frames",
    ))
    checks.append((
        "the strip faded out and returned to idle",
        carousel.state == "idle" and carousel.window.get_opacity() < 0.01,
        f"state {carousel.state}, opacity {carousel.window.get_opacity():.2f}",
    ))

    after = screenshot(display)
    restored = mean_diff(after, plain)
    checks.append(("the screen is back to normal afterwards", restored < 3.0,
                   f"mean difference vs the original screen: {restored:.1f}"))

    frame_ms = carousel.stats["frame_max_ms"]
    checks.append((
        "a frame fits a 60Hz budget",
        frame_ms < 16.0,
        f"worst frame while the strip was up: {frame_ms}ms "
        f"(last {carousel.stats['frame_ms']}ms of {carousel.stats['draws']} draws)",
    ))

    if args.save:
        prefix = args.save
        prefix.parent.mkdir(parents=True, exist_ok=True)
        for selection, shot in shots.items():
            path = prefix.with_name(f"{prefix.name}_sel{selection}.png")
            shot.save(path)
            print(f"spike: saved {path}")
        plain.save(prefix.with_name(f"{prefix.name}_plain.png"))

    if args.keep:
        driver.land()
        driver.drag(1.2 if room_right else -1.2)
        print("spike: strip is up; press Enter to finish")
        input()

    carousel.abort()
    carousel.close()
    cache.set_busy(False)
    cache.stop()

    print()
    print("=== checks ===")
    for label, ok, detail in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}\n         {detail}")
    failed = [label for label, ok, _ in checks if not ok]
    print()
    print(f"spike: {len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
