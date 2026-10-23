#!/usr/bin/env python3
"""Generate the golden fixtures the C++ suite checks itself against.

Both files are produced by the *Python* implementation, which is the reference:

    fixtures/steps_at.tsv   the commit rule, swept over progress at 1e-4 for
                            the shipped commit fraction and dense probes around
                            every half-step boundary for the others (this pins
                            the epsilon and the floor/ceil asymmetry)
    fixtures/layout.json    the strip's geometry for a matrix of screen sizes,
                            card counts and slot fractions

Regenerate only when the Python behaviour intentionally changes:

    python3 tools/make_fixtures.py
"""

from __future__ import annotations

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from wintab import overlay  # noqa: E402
from wintab.config import Config  # noqa: E402
from wintab.gesture import _steps_at  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "fixtures"

#: Progress step of the main sweep: fine enough to hit every half-step exactly.
SWEEP_STEP = 1e-4


def steps_fixture() -> str:
    lines = ["# commit_fraction progress steps   (generated from the Python build)"]
    # The shipped rule, swept end to end.
    commit = Config().commit_fraction
    steps = -3.0
    while steps <= 3.0 + 1e-9:
        progress = round(steps, 4)
        lines.append("%.4f %.17g %d" % (commit, progress, _steps_at(progress, commit)))
        steps += SWEEP_STEP
    # Every other commit fraction the app has shipped with, pinned hardest
    # around its boundaries (where the epsilon decides).
    for commit in (0.3, 0.35, 0.65, 1.0):
        lines.append("")
        for whole in range(-3, 4):
            for offset in (-2e-9, -1e-9, 0.0, 1e-9, 2e-9):
                progress = whole + 0.5 + offset
                lines.append("%.17g %.17g %d" % (commit, progress, _steps_at(progress, commit)))
    return "\n".join(lines) + "\n"


class StubCard:
    """Just enough card for the layout maths (no cairo surface needed)."""

    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height


def make_carousel(count: int, size: tuple[int, int], slot_fraction: float):
    config = Config()
    config.slot_fraction = slot_fraction
    strip = overlay.Carousel(config)
    strip._width, strip._height = size
    strip._layout()
    card = overlay.card_size(1440, 900, strip._slot_w, strip._row_h)
    strip._cards = [StubCard(*card) for _ in range(count)]
    strip._selection = 0
    strip._sel_float = 0.0
    strip._outline_w, strip._outline_h = float(card[0]), float(card[1])
    strip._view = strip._view_from = strip._view_to = 0.0
    return strip


def layout_fixture() -> str:
    screens = [(1920, 1080), (2560, 1440), (1366, 768), (3840, 2160), (800, 600)]
    counts = [1, 2, 4, 6, 9]
    fractions = [0.18, 0.22, 0.3]
    cases = []
    for screen in screens:
        for count in counts:
            for fraction in fractions:
                strip = make_carousel(count, screen, fraction)
                cases.append(
                    {
                        "screen": list(screen),
                        "count": count,
                        "slot_fraction": fraction,
                        "visible": strip._visible,
                        "slot_w": strip._slot_w,
                        "gap": strip._gap,
                        "pitch": strip._pitch,
                        "row_h": strip._row_h,
                        "card": [strip._cards[0].width, strip._cards[0].height],
                        "rects": [list(r) for r in strip.card_rects()],
                        "outline": list(strip.outline_rect()),
                        "band": [strip._band_top, strip._band_h],
                    }
                )
    return json.dumps(cases, indent=1) + "\n"


def main() -> int:
    FIXTURES.mkdir(exist_ok=True)
    (FIXTURES / "steps_at.tsv").write_text(steps_fixture())
    (FIXTURES / "layout.json").write_text(layout_fixture())
    print(f"wrote {FIXTURES / 'steps_at.tsv'}")
    print(f"wrote {FIXTURES / 'layout.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
