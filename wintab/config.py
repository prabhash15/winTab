"""Configuration for winTab.

Everything tunable lives here. Defaults are chosen so the gesture feels like
Windows 11's window strip on a ~128mm wide touchpad; every distance is
expressed as a fraction of the touchpad width (or the screen) so the same
config works on other hardware.
"""

from __future__ import annotations

import dataclasses
import os
import pathlib
import tomllib

CONFIG_DIR = pathlib.Path(
    os.environ.get("XDG_CONFIG_HOME") or (pathlib.Path.home() / ".config")
) / "wintab"
CONFIG_PATH = CONFIG_DIR / "config.toml"

#: Values ``order`` accepts; anything else falls back to the first one.
ORDER_MODES = ("opened", "recent")


@dataclasses.dataclass
class Config:
    # --- gesture feel -----------------------------------------------------
    # Finger travel per card, as a fraction of the touchpad width. 0.095 is
    # ~12mm on a 128mm pad, so the outline moves to the next window after ~6mm
    # of travel and a comfortable swipe (a third of the pad) crosses three
    # cards. Raise it if a swipe overshoots the window you meant; lower it if
    # reaching the far end of a long strip feels like hard work.
    step_fraction: float = 0.095
    # Movement ignored before we decide the swipe is real (avoids jitter and
    # keeps three-finger taps from opening the strip).
    dead_zone_fraction: float = 0.025
    # How much more horizontal than vertical the movement must be.
    direction_dominance: float = 1.15
    # How far past a card you must travel to select/open it. This is the only
    # rule that decides the commit, and the outline uses the same one, so the
    # outlined card is always the one that opens -- speed cannot change it.
    commit_fraction: float = 0.5
    # Resistance when tugging past the first/last window (1.0 = none).
    rubber_band: float = 0.35
    # Swap the direction (right selects the window to the left) if it feels
    # backwards on your pad.
    invert_direction: bool = False
    # All three fingers must land within this many milliseconds.
    arm_window_ms: float = 40.0
    # A 3-finger tap is this short and this still; it never opens the strip.
    tap_max_ms: float = 300.0
    tap_max_move: float = 0.015

    # --- carousel layout --------------------------------------------------
    # Width of each card's slot, as a fraction of the screen width. 0.22 fits
    # four cards across a 1920px screen, like Windows 11's window strip.
    slot_fraction: float = 0.22
    # Space between slots, as a fraction of the screen width.
    card_gap_fraction: float = 0.018
    # Tallest a card can be, as a fraction of the screen height. A window with
    # a squarer shape is limited by its slot width instead, and its aspect
    # ratio is always preserved.
    max_card_height_fraction: float = 0.40
    # The lighter band the row sits on (Windows 11 draws one behind the cards).
    band_alpha: float = 0.12
    # How much taller than the row the band is, in pixels.
    band_pad_px: int = 26
    card_radius: float = 8.0
    # Width of the soft shadow baked around each card.
    card_shadow_px: float = 26.0
    # How much the cards you are *not* on are darkened.
    dim_others: float = 0.18

    # --- the selection outline -------------------------------------------
    # Windows' accent blue.
    highlight_rgb: list = dataclasses.field(default_factory=lambda: [0.0, 0.47, 0.83])
    highlight_px: float = 3.0
    # Distance between the outline and the card's edge.
    highlight_gap_px: float = 4.0
    # How long the outline takes to slide from one card to the next.
    highlight_ms: int = 120
    # The name of the application, drawn under its card (Windows 11 labels
    # every thumbnail). 0 hides it.
    caption_font_px: float = 13.0
    caption_gap_px: float = 6.0
    # A small sideways tug of the whole row as you drag, in card widths
    # (0 = the row stays perfectly still and only the outline moves).
    row_parallax: float = 0.0

    # --- visuals ----------------------------------------------------------
    # How much the backdrop is darkened (Windows 11 dims and blurs it).
    dim: float = 0.30
    blur_radius: float = 6.0
    blur_downscale: int = 8
    fade_ms: int = 120

    # --- model ------------------------------------------------------------
    # One card per window (Windows 11's strip behaviour). Set true to group
    # every window of an application into a single card instead.
    group_by_app: bool = False
    # Left-to-right order of the cards:
    #   "recent" -- most recently used first, like Alt+Tab: the window you are
    #               on is card 0, the one you were on before it is card 1, and
    #               the row re-sorts after every switch, so the next swipe
    #               always starts from where you are.
    #   "opened" -- the order the windows were first opened. The row stays
    #               still between gestures, so a window keeps its place and the
    #               focused window can sit anywhere in the row.
    order: str = "recent"

    # --- capture ----------------------------------------------------------
    # Forget the screen settled for this long before grabbing a new snapshot.
    capture_debounce_s: float = 0.4
    # Fallback poll interval used if the DAMAGE extension is unavailable.
    capture_poll_s: float = 5.0

    # --- input ------------------------------------------------------------
    # Substring of the input device name. None = first device named *Touchpad*.
    touchpad_name: str | None = None
    # Suppress pointer motion while a gesture is active (X11 pointer grab).
    freeze_pointer: bool = False

    # --- debug ------------------------------------------------------------
    verbose: bool = False
    show_fps: bool = False

    def as_dict(self) -> dict:
        return dataclasses.asdict(self)

    @property
    def highlight_color(self) -> tuple[float, float, float]:
        """``highlight_rgb`` as a clamped 3-tuple, so a typo cannot crash a draw."""
        values = list(self.highlight_rgb or [])[:3]
        while len(values) < 3:
            values.append(0.0)
        return tuple(min(1.0, max(0.0, float(v))) for v in values)


def load(path: pathlib.Path | None = None) -> Config:
    """Load config.toml, ignoring unknown keys so old files keep working."""
    cfg = Config()
    path = path or CONFIG_PATH
    try:
        with open(path, "rb") as fh:
            data = tomllib.load(fh)
    except FileNotFoundError:
        return cfg
    except (OSError, tomllib.TOMLDecodeError) as exc:
        print(f"wintab: ignoring {path}: {exc}")
        return cfg

    known = {f.name for f in dataclasses.fields(Config)}
    for key, value in data.items():
        if key not in known:
            print(f"wintab: unknown config key {key!r} ignored")
            continue
        if key == "order" and str(value) not in ORDER_MODES:
            print(
                f"wintab: order {value!r} is not one of "
                f"{'/'.join(ORDER_MODES)}; using {cfg.order!r}"
            )
            continue
        setattr(cfg, key, value)
    return cfg
