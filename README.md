# winTab

Windows 11 style window switching for Linux Mint / Cinnamon on X11: put three
fingers on the touchpad and drag left or right. A row of cards comes up -- one
per open window in this workspace, each showing the real window -- with the
current one outlined. Move your fingers right and the outline moves right. Let
go and that window opens.

> Want to see it without installing? `python3 tools/spike_carousel.py --keep`
> drives a synthetic swipe and leaves the strip on screen until you press Enter.

## What it does, exactly

* **three fingers, left or right** open the strip and move the selection
* **one card per open window** in the current workspace, each card that
  window's *own* pixels -- including its title bar, and regardless of what is
  stacked on top of it -- with the application's name underneath. The row is
  sorted most-recently-used first, like Alt+Tab: the window you are on is the
  first card, and the row re-sorts after every switch, so a swipe always starts
  from where you are (`order = "opened"` keeps the row still instead)
* **workspaces are followed, not cached.** Switch workspace and the strip is
  that workspace's windows: a screen grab is stamped with the desktop it was
  taken on, and a card is never cropped out of a grab from another one -- for
  the ~0.4s until a grab of the new workspace lands, that window shows a blank
  card with its name rather than somebody else's pixels. The strip itself is
  one sticky window that the manager draws on *every* workspace -- a window
  left on the one it was created in is invisible everywhere else while the
  gesture keeps working, which is exactly how "it only shows up in the
  workspace I started it in" looked
* **moving right selects the window to the right** (set `invert_direction = true`
  to swap it)
* the selected card is outlined in Windows' accent blue and drawn at full
  brightness; the others are slightly darkened
* **nothing opens while your fingers are down.** Lifting opens whatever is
  outlined -- always exactly that card, at any speed; lifting after less than
  half a card of travel keeps you where you were
* at the first or last window the selection simply stops, with a little give
* more windows than fit on screen: the row scrolls in card steps to keep the
  selection in view
* minimized windows still get a card, drawn as an empty frame because there are
  no on-screen pixels to show

Deliberately **not** included: three-finger up/down, three-finger tap, four
fingers, workspace switching, Expo/Task View, or a settings GUI. If you want
those, the machinery here (raw contacts + a pre-mapped overlay) is a good
starting point, but they are out of scope on purpose.

## Requirements

* X11 with a compositing window manager (Cinnamon/Muffin is what it is built and
  tested against). Wayland will not work: it needs raw evdev and EWMH.
* Linux Mint 22.x / Cinnamon 6.x, Python 3.11+ (`tomllib`)
* `python3-gi`, `gir1.2-gtk-3.0`, `python3-cairo`, `python3-pil`, `python3-xlib`
* your user in the `input` group, so `/dev/input/event*` is readable without
  root. No setuid helpers, no daemon, no `sudo` at runtime.
  `sudo usermod -aG input $USER` and log back in.

## Quick start

```bash
python3 -m wintab --probe 20     # three fingers on the pad -> expect PASS
python3 -m wintab --dry-run      # swipe: prints what it *would* do, draws nothing
python3 -m wintab                # for real
```

`--probe` is not optional ceremony. It is the one thing that proves your pad
reports three *simultaneous* contacts, which the whole design depends on. Add
`--record log.jsonl` to save the frames, and `python3 -m wintab --replay
log.jsonl` to replay them later without touching the pad.

## Install

```bash
./install.sh              # config + autostart entry + preflight checks
./install.sh --uninstall  # remove the autostart entry again
```

It never runs `sudo`. It copies `config.example.toml` to
`~/.config/wintab/config.toml` (only if you do not have one), writes a
`~/.config/autostart/wintab.desktop`, and tells you about anything else that
wants the three-finger swipe.

## Configuration

Every tunable lives in `~/.config/wintab/config.toml`;
`config.example.toml` is fully commented. `python3 -m wintab --print-config`
shows what is actually in effect, including defaults you did not write.

The ones you are most likely to touch:

| key | default | what it does |
| --- | --- | --- |
| `step_fraction` | `0.13` | finger travel per card, as a fraction of the pad width (~17mm) |
| `commit_fraction` | `0.5` | how far past a card selects it; also where the outline flips |
| `invert_direction` | `false` | swap which way the windows go |
| `slot_fraction` | `0.22` | card width, as a fraction of the screen |
| `max_card_height_fraction` | `0.40` | tallest a card can be (aspect is preserved) |
| `dim_others` | `0.18` | how much the unselected cards are darkened |
| `band_alpha` | `0.12` | the lighter band behind the row (`0` to remove it) |
| `row_parallax` | `0.0` | `0.25` for a small sideways tug of the row while dragging |
| `highlight_ms` / `fade_ms` | `120` / `120` | outline slide, and the fade back |
| `caption_font_px` / `caption_gap_px` | `13.0` / `6.0` | the name under each card (`0` hides it) |
| `group_by_app` | `false` | `true` collapses each app's windows into one card |
| `order` | `"recent"` | card order: `"recent"` is Alt+Tab order (current window first), `"opened"` keeps the row still |
| `freeze_pointer` | `false` | hold the cursor still during a swipe |
| `verbose` / `show_fps` | `false` | print decisions / draw a frame-time readout |

## How it works

Seven decisions, each one there because the obvious alternative was too slow,
wrong, or would not have worked:

* **Raw evdev touches, not gestures.** libinput only hands out already-parsed
  *gesture* events, which is exactly what a finger-following selection must not
  use. `wintab/touchpad.py` reads the multitouch protocol straight from
  `/dev/input/event*` (per-slot contact positions at the pad's own ~80Hz), with
  no extra privileges.
* **A pure state machine.** `wintab/gesture.py` takes contact frames and emits
  `SwipeBegin`/`Update`/`End` events. No X11, no GTK, no threads -- so the rules
  (fingers must land together, never four, horizontal only, dead zone, end
  stops) are covered by unit tests with synthetic frames. Each `SwipeUpdate`
  carries the window the progress points at, computed with the *same* rule the
  commit uses -- speed is not an input to that rule -- so the outlined card and
  the opened card cannot drift apart, not even on a flick.
* **Pixels prepared ahead of time.** Grabbing 1920x1080 off the X server costs
  ~60ms and the blur another ~60ms, far too slow for a gesture. `capture.py`
  keeps a snapshot and a dimmed, blurred backdrop warm in a worker thread, woken
  by the X DAMAGE extension (arm after every capture, disarm on the first
  change) rather than by polling: ~130ms per capture, 0.4% of one core while
  idle. The carousel then crops and scales each window into a card in that same
  worker thread -- ~176ms for six windows, never during a swipe: the worker
  checks `busy` between windows and abandons the rest the moment a gesture
  starts, and the app refuses to bake at all while the strip is up, so no
  background work can land in the middle of the frames you are watching.
* **Nothing on the gesture path talks to the server more than it must.**
  Opening the strip used to enumerate every window twice (~50ms of round trips
  on the GTK thread, right as the strip was coming up). The window list is now
  reused for half a second, so those two passes are one; a window's class and
  pid are read once and remembered (they cannot change without the window being
  replaced); atom names are cached, since a name never changes; and the place of
  a window on screen is asked for once instead of twice. Measured: the strip's
  opening latency fell from 51-74ms to 12-20ms, with the row itself rendering in
  ~4.7ms a frame. The idle poll also only re-reads the window list when the
  capture thread has actually seen the screen change, which is what keeps a
  stall from landing on the first frames of a swipe.
* **A row that re-sorts itself.** Cards are laid out most-recently-used first
  -- the Alt+Tab order, falling back to `_NET_CLIENT_LIST` for windows nobody
  has focused yet -- so the window you are on is card 0 and a swipe always
  starts from where you are. The cost is that a window's place changes between
  gestures; `order = "opened"` freezes the row in the order the windows were
  first opened instead, which is a calmer picture but can leave the focused
  window anywhere in it.
* **Each card is the window's own pixels.** Cropping the window's place out of
  the screen grab is free but wrong the moment something overlaps it -- the card
  then shows whatever is in front. `capture.py` asks the compositor for the
  window's off-screen pixmap instead (23ms per window, on the capture thread),
  through the *frame* the window manager reparented it into, since only the
  root's own children are redirected. Windows that nothing covers, and that sit
  wholly on screen, still use the free crop: only the ones where a crop would
  lie pay for the read. A capture that cannot read a single window keeps the
  pixels it already has rather than emptying the strip. Because a crop is only as good as the grab it came from,
  every grab is stamped with its workspace, and switching workspace asks the
  capture thread to hurry (waking it out of its `select`, capping its settle
  wait at 250ms) and marks every window as needing its own pixels until a grab
  of the new workspace lands -- 0.4s measured, and never a card from the
  workspace you left.
* **Switching quotes the current time, not a remembered one.** EWMH's
  `_NET_ACTIVE_WINDOW` carries a timestamp, and window managers quietly refuse a
  request older than the last one they saw -- focus-stealing prevention. Sending
  the time of the last property change we happened to notice looks reasonable
  and is a trap: a minute later the request is refused, nothing changes, so no
  new property change arrives to refresh that timestamp, and switching stops
  working for good -- silently, because the app only knew it had *sent* the
  request. It sends `CurrentTime` (the gesture is a user action happening now,
  and raw evdev touches have no X timestamp), checks afterwards that the window
  really took focus, and says so in the log and asks once more if it did not.
* **Cards baked twice, bright and dimmed.** Choosing what to draw per frame is
  then a pointer swap, not a blur or an overlay.
* **Nothing is redrawn while nothing changes.** A motionless three-finger hold
  still reports ~80 frames a second; the carousel ignores sub-pixel movement, so
  holding still costs **zero** frames. A window that is never mapped or unmapped
  (the overlay is created at startup, kept mapped at zero opacity, and shown
  with a single `set_opacity`) keeps the strip instant to raise.

The measured numbers on the machine this was built for: **~160ms** per capture
(the grab plus the blurred backdrop), **~23ms** per window read from the
compositor and **~107ms** to bake the cards -- all of it on the capture thread,
while the screen is settled and no gesture is on screen -- and a **8.3ms** worst
frame while the strip is up, against a 16.6ms budget for 60Hz. One thing worth knowing: Pillow's LANCZOS costs 47ms shrinking a
full-screen window to a card and rings while doing it, so
`thumbnail_resize()` halves with a box filter first (~11ms) and finishes with
LANCZOS.

## Layout

```
wintab/config.py     tunables + TOML loading
wintab/touchpad.py   raw evdev multitouch reader, device discovery, replay
wintab/gesture.py    the three-finger swipe state machine (pure)
wintab/apps.py       EWMH window model, workspace filter, card order, activation
wintab/capture.py    DAMAGE-driven screen snapshots, per-window pixels, backdrop
wintab/overlay.py    the pre-mapped card strip and its Cairo renderer
wintab/pointer.py    optional X pointer grab so the cursor holds still
wintab/app.py        wiring: threads, main loop, decisions, preflight checks
wintab/__main__.py   the command line
tools/probe.py       the three-contact hardware gate (detailed)
tools/probe_windows.py   where each window really is, and does switching work
tools/spike_overlay.py   proves the window recipe under Muffin
tools/probe_sticky.py    is the strip actually drawn on the workspace you swiped on
tools/spike_carousel.py  drives a synthetic swipe and checks the pixels on screen
```

## Troubleshooting

**A three-finger swipe tiles a window / switches workspaces instead.** Something
else owns the gesture. Both are checked at startup and `./install.sh` reports
them:

* *Touchegg* (default on Mint) maps three-finger left/right to `TILE_WINDOW`.
  Disable it completely: `sudo systemctl disable --now touchegg`. Or keep it and
  write `~/.config/touchegg/touchegg.conf` with everything **except** the
  three-finger left/right entries, so it still handles your other gestures.
* Cinnamon's own gestures map three-finger left/right to workspace switching
  even while its GUI looks disabled-empty:
  `dconf write /org/cinnamon/gestures/enabled false`.
  Prefer `gsettings`? Call `/usr/bin/gsettings` by full path: a Homebrew
  `gsettings` earlier on your PATH writes to a keyfile that Cinnamon never
  reads, so the change appears to succeed and does nothing.

**Nothing happens at all.** Run `python3 -m wintab --probe 20` with three
fingers on the pad. If it reports fewer than three contacts, the pad is the
problem, not the code. If it reports "permission denied", you are not in the
`input` group (or have not logged back in after joining).

**A window switches but does not come to the front, or only the first swipe or
two work.** That was a real bug once: the activation request quoted a stale
timestamp and the window manager refused it, with nothing in the log to say so.
Both are covered now -- the app sends `CurrentTime`, checks that the window
actually took focus, and logs `did not take focus ... asking again` if a window
manager still says no. A window deliberately set *always on top* will of course
stay above the one you switch to; that one is the window manager's call.

**The strip appears but the selection will not move.** You need to travel half a
card to tip it over; lower `commit_fraction` (e.g. `0.35`) to make it twitchier,
or lower `step_fraction` so a card takes less finger travel.

**It moves too far / not far enough per swipe.** `step_fraction` is the knob:
lower means more windows per finger-travel.

**The cards are too small / too many fit on screen.** `slot_fraction`
(and `max_card_height_fraction` for how tall they get).

**The direction feels backwards.** `invert_direction = true`.

**I want the motion to feel more connected to my fingers.**
`row_parallax = 0.25` tugs the whole row sideways as you drag.

**The cursor drifts while I swipe.** `freeze_pointer = true`.

**Everything is slow or the animation stutters.** Run with `--fps` for the
frame-time readout, and lower `blur_downscale`, `card_shadow_px`, or
`blur_radius`.

**The cards are empty for a moment after switching workspaces.** That is the
honest half-second: a screen grab belongs to one workspace, so a card cropped
from the grab you left is never used -- the window shows its name with no
picture until a grab of the new workspace arrives (0.4s on this machine). If
windows stay blank there, check `python3 -m wintab --verbose` for the cache
line: `'windows': 0` means the compositor handed over nothing for them.

**A card shows the wrong thing / a preview looks like a random screenshot.**
Run `python3 tools/probe_windows.py --workspace`: it prints each window's place
on screen next to the one the app computed, and compares the crop against the
window's own pixels. A window-relative position (a few tens of pixels, rather
than the window's real corner) is the bug this tool exists to catch.

## Tests

```bash
python3 -m unittest discover -s tests -t .   # 151 unit tests, no display needed
python3 tools/spike_carousel.py               # the visual checks, on the real screen
python3 tools/spike_overlay.py                # the window-recipe checks
python3 tools/probe_windows.py --activate     # where windows are, and switching
```

The unit tests cover the gesture engine, the window model (the card order, which
card the outline opens on, where a window really is on screen and which cards
need their own pixels), the whole strip layout (slot spacing, centring,
scrolling, aspect fitting, where the outline lands) and what a card is built
from. `spike_carousel.py` synthesises touchpad frames, feeds them through the
**real** state machine and applies the events the way `app.py` does, then shows
the result briefly on your screen and checks 20 things: that the cards are where
the layout says and show **the window itself** rather than whatever is stacked
on it, that every listed window is placed where it really is, that each card
carries its own application's name, that the outline is drawn in accent blue
exactly one slot apart, that the selected card is brighter and the row sits on a
band, that **nothing is activated while the fingers are down**, that a
sub-half-step release changes nothing, that holding still costs no redraws, and
that the screen is bit-identical afterwards. It never activates a window, and
`probe_windows.py --activate` is the tool for the other half: it walks the
workspace asking the window manager to activate each window with the app's own
request and reports whether focus followed.

## Known limits

* Three-finger gestures are only as good as your pad's reporting. The pad this
  was built against (`HTIX5288:00 36B6:C001`) was verified to report three
  simultaneous contacts on every frame; `--probe` is there so you can check
  yours before blaming the code.
* Cards sort by focus history by default (`order = "recent"`), like Alt+Tab:
  the focused window is card 0 and the row re-sorts after every switch, at the
  cost of the row rearranging itself between gestures. `order = "opened"`
  freezes the row in the order the windows were first opened -- the same picture
  every time -- but then the focused window can sit anywhere in it.
* Multi-monitor is implemented (coordinates are root-relative and the whole root
  is captured) but has not been tested on a real second screen.
* The very first frame of the very first swipe of a session can take ~40ms while
  the compositor first composites the newly-visible overlay; every frame after
  that is ~8ms.
* A card read from the compositor is the window's frame, which is a border wider
  than `_NET_FRAME_EXTENTS` reports; the card simply includes that border.
* Minimized windows have no pixels anywhere -- the compositor has nothing to
  hand over -- so their card is an empty frame with the name under it.
* `freeze_pointer` is off by default and lightly tested; X drops the grab if the
  process dies, so it cannot leave your mouse stuck.
