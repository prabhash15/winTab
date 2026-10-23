# winTab

Windows 11 style window switching for Linux Mint / Cinnamon on X11: put three
fingers on the touchpad and drag left or right. A row of cards comes up -- one
per open window in this workspace, each showing the real window -- with the
current one outlined. Move your fingers right and the outline moves right. Let
go and that window opens.

> Want to see it without installing? `./build/spike_carousel --keep` drives a
> synthetic swipe and leaves the strip on screen until you press Enter.

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
* Linux Mint 22.x / Cinnamon 6.x, a C++20 compiler and CMake 3.20+
* the GTK 3, Cairo and Xlib development headers: `libgtk-3-dev`,
  `libcairo2-dev`, `libx11-dev`, `libxdamage-dev`, `libxcomposite-dev`
  (on Mint the last four come in with `libgtk-3-dev`)
* your user in the `input` group, so `/dev/input/event*` is readable without
  root. No setuid helpers, no daemon, no `sudo` at runtime.
  `sudo usermod -aG input $USER` and log back in.

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That produces `build/wintab` (the app), `build/wintab_tests` (the unit suite),
and the probes: `build/spike_carousel`, `build/spike_overlay`,
`build/probe_windows`, `build/trace_replay`. Nothing is installed until you
ask:

```bash
./install.sh                             # builds and installs to ~/.local/bin
PREFIX=/opt/wintab ./install.sh          # somewhere else
cmake --install build --prefix ~/.local  # just the binary
```

## Quick start

```bash
./build/wintab --probe 20     # three fingers on the pad -> expect PASS
./build/wintab --dry-run      # swipe: prints what it *would* do, draws nothing
./build/wintab                # for real
```

`--probe` is not optional ceremony. It is the one thing that proves your pad
reports three *simultaneous* contacts, which the whole design depends on. Add
`--record log.jsonl` to save the frames, and `--replay log.jsonl` to replay
them later without touching the pad. Recordings are plain JSONL, so a trace
recorded with the old Python build still replays here.

## Install

```bash
./install.sh              # build + binary + config + autostart + preflight
./install.sh --uninstall  # remove the autostart entry again
```

It never runs `sudo`. It checks the build dependencies, builds the binary,
installs it to `~/.local/bin` (or `$PREFIX/bin`), copies
`config.example.toml` to `~/.config/wintab/config.toml` (only if you do not
have one), writes a `~/.config/autostart/wintab.desktop` pointing at the
absolute path of the binary, and tells you about anything else that wants the
three-finger swipe.

## Configuration

Every tunable lives in `~/.config/wintab/config.toml`;
`config.example.toml` is fully commented. `wintab --print-config` (or
`./build/wintab --print-config` before installing) shows what is actually in
effect, including defaults you did not write. The C++ build keeps every key of
the Python build, with the same names, types and defaults.

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
  use. `src/touchpad.cpp` reads the multitouch protocol straight from
  `/dev/input/event*` (per-slot contact positions at the pad's own ~80Hz), with
  no extra privileges.
* **A pure state machine.** `src/gesture.cpp` takes contact frames and emits
  `SwipeBegin`/`Update`/`End` events. No X11, no GTK, no threads -- so the rules
  (fingers must land together, never four, horizontal only, dead zone, end
  stops) are covered by unit tests with synthetic frames. Each `SwipeUpdate`
  carries the window the progress points at, computed with the *same* rule the
  commit uses -- speed is not an input to that rule -- so the outlined card and
  the opened card cannot drift apart, not even on a flick.
* **Pixels prepared ahead of time.** Grabbing 1920x1080 off the X server and
  blurring it is far too slow to do at the start of a gesture. The grabs go
  through MIT-SHM (`XShmGetImage`), so the server writes straight into a buffer
  this process already owns instead of copying every pixel down the socket:
  measured 1ms instead of 25ms for the root window and ~2.5x per window, with the
  old path kept as the fallback for a display without shared memory. `src/capture.cpp`
  keeps a snapshot and a dimmed, blurred backdrop warm in a worker thread, woken
  by the X DAMAGE extension (arm after every capture, disarm on the first
  change) rather than by polling, so the worker sleeps when nothing moves. The
  carousel then crops and scales each window into a card in that same worker
  thread, never during a swipe: the worker checks `busy` between windows and
  abandons the rest the moment a gesture starts, and the app refuses to bake at
  all while the strip is up, so no background work can land in the middle of the
  frames you are watching.
* **Nothing on the gesture path talks to the server more than it must.**
  Opening the strip used to enumerate every window twice (~50ms of round trips
  on the GTK thread, right as the strip was coming up). The window list is now
  reused for half a second, so those two passes are one; a window's class and
  pid are read once and remembered (they cannot change without the window being
  replaced); atom names are cached, since a name never changes; and the place of
  a  window on screen is asked for once instead of twice. The idle poll also only
  re-reads the window list when the
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
  then shows whatever is in front. `src/capture.cpp` asks the compositor for the
  window's off-screen pixmap instead (one read per window, on the capture thread),
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

Measured on this machine with three windows open (two runs, the numbers the
tools themselves report -- so they are reproducible):

| step | Python build | C++ build |
| --- | --- | --- |
| full-screen grab + backdrop (`capture_ms`) | 212-340ms | 122-158ms |
| bake the cards for the row (`prewarm`) | 113-182ms | 92-107ms |
| worst frame while the strip is up | 12.7-15.1ms | 13ms |

All of that capture and baking happens on the capture thread while the screen is
settled and no gesture is on screen; the frame budget for 60Hz is 16.6ms. What a
capture still costs is now the backdrop (61ms: 43ms building it with the bicubic
upscale, 16ms the bilinear downscale) and the one copy of each grab into a
cairo-owned buffer (~5ms per 1920x1080 surface); the transfer itself is nearly
gone. The card bake is where the in-process pixel kernels win, because there is
no Pillow round trip any more.
`thumbnail_resize()` still halves with a box filter first and finishes with
Lanczos -- the same recipe, so cards look the same.

## Layout

```
src/main.cpp         the command line, probe loop
src/config.cpp       tunables + TOML loading
src/toml_lite.cpp    the small TOML parser (flat keys, as shipped)
src/touchpad.cpp     raw evdev multitouch reader, device discovery, replay
src/gesture.cpp      the three-finger swipe state machine (pure)
src/x11.cpp          the X11 seam (XBackend) and its Xlib backend
src/windows.cpp      EWMH window model, workspace filter, card order, activation
src/capture.cpp      DAMAGE-driven screen snapshots, per-window pixels, backdrop
src/cards.cpp        card baking: aspect fit, shadow, placeholder, dim twin
src/overlay.cpp      the pre-mapped card strip and its Cairo renderer
src/pointer.cpp      optional X pointer grab so the cursor holds still
src/app.cpp          wiring: threads, main loop, decisions, preflight checks
src/image.cpp        pixel kernels replacing Pillow (crop, resize, blur, blend)
tools/probe_windows.cpp   where each window really is, and does switching work
tools/spike_carousel.cpp  drives a synthetic swipe and checks the pixels on screen
tools/spike_overlay.cpp   proves the overlay window recipe under the compositor
tools/trace_replay.cpp    replays a recording through the gesture engine
tests/                   the unit suite (harness + fakes)
wintab/                  the Python reference build, kept until cutover sign-off
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

**Nothing happens at all.** Run `wintab --probe 20` with three
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
windows stay blank there, check `wintab --verbose` for the cache
line: `'windows': 0` means the compositor handed over nothing for them.

**A card shows the wrong thing / a preview looks like a random screenshot.**
Run `./build/probe_windows`: it prints each window's place
on screen next to the one the app computed, and compares the crop against the
window's own pixels. A window-relative position (a few tens of pixels, rather
than the window's real corner) is the bug this tool exists to catch.

## Tests

```bash
./build/wintab_tests                                       # 161 unit tests, no display needed
./build/spike_carousel                                     # 20 visual checks, real screen
./build/spike_carousel --keep                              # leave the strip up
./build/spike_overlay                                      # the window recipe
./build/probe_windows --activate                           # where windows are, switching
python3 tools/parity_check.py --cxx build/trace_replay      # 200 fuzzed streams vs Python
```

The last one is the differential check: it synthesises touchpad streams, runs
them through both the Python reference engine and the C++ one, and fails on a
single diverging event. `build/trace_replay` is the C++ half.

The unit tests cover the gesture engine, the window model (the card order, which
card the outline opens on, where a window really is on screen and which cards
need their own pixels), the whole strip layout (slot spacing, centring,
scrolling, aspect fitting, where the outline lands) and what a card is built
from. `spike_carousel` synthesises touchpad frames, feeds them through the
**real** state machine and applies the events the way `app.cpp` does, then shows
the result briefly on your screen and checks 20 things: that the cards are where
the layout says and show **the window itself** rather than whatever is stacked
on it, that every listed window is placed where it really is, that each card
carries its own application's name, that the outline is drawn in accent blue
exactly one slot apart, that the selected card is brighter and the row sits on a
band, that **nothing is activated while the fingers are down**, that a
sub-half-step release changes nothing, that holding still costs no redraws, and
that the screen is bit-identical afterwards. It never activates a window, and
`probe_windows --activate` is the tool for the other half: it walks the
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
* The Python build is still in the tree under `wintab/` as the reference for the
  differential checks. The C++ binary does not import it; deleting the directory
  is the last cutover step, after sign-off.
* The very first frame of the very first swipe of a session can take ~40ms while
  the compositor first composites the newly-visible overlay; every frame after
  that is ~8ms.
* A card read from the compositor is the window's frame, which is a border wider
  than `_NET_FRAME_EXTENTS` reports; the card simply includes that border.
* Minimized windows have no pixels anywhere -- the compositor has nothing to
  hand over -- so their card is an empty frame with the name under it.
* `freeze_pointer` is off by default and lightly tested; X drops the grab if the
  process dies, so it cannot leave your mouse stuck.
