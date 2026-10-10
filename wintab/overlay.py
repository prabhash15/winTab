"""The carousel: a pre-mapped window showing every open window as a row of cards.

This is Windows 11's window strip, not a sliding desktop. When three fingers
move, the overlay comes up with a blurred, dimmed screen, a slightly lighter
band across the middle, and one card per open window in this workspace laid out
left to right. A card is a real, frozen picture of that window -- title bar and
all. The window you are on is outlined; moving your fingers right moves the
outline right, and **nothing opens until you let go**.

Four decisions keep this cheap enough to feel instant on a machine with no GPU
acceleration to speak of:

* the window is created, mapped at zero opacity and *never unmapped*, so
  showing the strip costs one ``set_opacity`` instead of a map/unmap round trip
  through the compositor
* every card's pixels are built ahead of time, off the gesture path: the capture
  worker hands us a fresh screen snapshot and we crop and scale each window once
  (``prewarm``). Cards are ~0.1 megapixels where the old full-screen sheets were
  2, so this is an order of magnitude cheaper
* each card is baked twice, bright and dimmed, so choosing which one to draw
  costs nothing per frame
* while the fingers are down and the selection is not changing, **nothing is
  redrawn at all** -- a motionless three-finger hold costs zero frames

Everything here runs on the GTK main thread except ``prewarm``, which is called
from the capture thread and touches no X11 objects of ours -- only PIL images and
cairo surfaces -- because python-xlib is not safe to use from two threads at
once.

The window itself is configured exactly the way ``tools/spike_overlay.py``
proved under Muffin: undecorated NOTIFICATION window, keep-above, never
focused, empty input shape (every click falls through to the app below), and
stuck to **every** workspace. That last one is not decoration: a window is
composited only on the workspace its manager put it on, which is how the strip
ends up invisible everywhere except the one winTab was started in, while the
gesture keeps working. ``realize_overlay`` is where it is arranged: the app
writes ``_NET_WM_DESKTOP`` while the window is realized but not yet mapped.
"""

from __future__ import annotations

import dataclasses
import math
import time

import cairo
import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

from PIL import Image  # noqa: E402

from .gesture import apply_rubber_band, highlight_index  # noqa: E402
from .apps import clear_attention

IDLE = "idle"
DRAG = "drag"
FADE = "fade"

_SLIDE_DECAY = 3.0

#: How often, and how many times, the sticky request is repeated after the
#: overlay is first mapped. The map itself has to be finished -- the window has
#: to be one the manager knows -- before it will accept the request at all.
STICKY_RETRY_MS = 400
STICKY_RETRIES = 3

#: The attention flag is stamped on when the manager first takes the window
#: on, and again after every sticky re-assertion -- so it is cleared at exactly
#: those moments, plus after the first ``open()`` in case the manager came back
#: late (a compositor restart against a running winTab). Left standing, the
#: flag makes every newly opened application "open behind": Cinnamon's focus
#: stealing prevention sees a window-that-wants-attention and routes the new
#: window into flashing instead of raising it.
ATTENTION_CLEAR_DELAY_MS = 1200
ATTENTION_CLEAR_RETRIES = 4


def caption_baseline(card_top: float, card_height: float, gap_px: float,
                     font_px: float) -> float:
    """Where the name under a card sits, as a Cairo baseline.

    ``0.8`` of the font size is the usual ascender-to-baseline ratio, which is
    close enough for a one-line label and keeps this a pure function.
    """
    return card_top + card_height + gap_px + font_px * 0.8


def ease_out(t: float) -> float:
    """Cubic-out: ``t`` in 0..1 in, 0..1 out."""
    t = min(1.0, max(0.0, t))
    return 1.0 - (1.0 - t) ** _SLIDE_DECAY


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def _rounded_rect(ctx: cairo.Context, x: float, y: float, w: float, h: float, r: float) -> None:
    if w <= 0 or h <= 0:
        return
    r = max(0.0, min(r, w / 2.0, h / 2.0))
    if r <= 0.5:
        ctx.rectangle(x, y, w, h)
        return
    ctx.new_sub_path()
    ctx.arc(x + w - r, y + r, r, -math.pi / 2.0, 0.0)
    ctx.arc(x + w - r, y + h - r, r, 0.0, math.pi / 2.0)
    ctx.arc(x + r, y + h - r, r, math.pi / 2.0, math.pi)
    ctx.arc(x + r, y + r, r, math.pi, 1.5 * math.pi)
    ctx.close_path()


def realize_overlay(win, make_sticky=None):
    """Realize the overlay *before* mapping it, then hand its X id to the app.

    The order carries the meaning. A window manager reads ``_NET_WM_DESKTOP``
    once, when it first maps a window, and a window it has placed on one
    workspace is not composited on any other one -- so the strip, created at
    startup and never unmapped, has to say it lives on *every* workspace while
    it still exists only on this side of the screen. ``make_sticky`` is how the
    app says it (it owns the X connection); it is optional so the layout tests
    can build a carousel with no X server anywhere.

    Half the request can only be made here and the other half only after the
    window is mapped -- see ``map_overlay`` -- so the id goes over twice.

    Returns the realized ``Gdk.Window``.
    """
    win.realize()
    gdk_window = win.get_window()
    if gdk_window is None:
        raise RuntimeError("overlay window was not created")
    if make_sticky is not None:
        make_sticky(int(gdk_window.get_xid()))
    return gdk_window


def map_overlay(win, gdk_window, make_sticky=None) -> None:
    """Map the overlay, then ask for every workspace again -- now it counts.

    The first half of the sticky request is the ``_NET_WM_DESKTOP`` property,
    which the manager reads at the window's first map. The second half is the
    ``_NET_WM_STATE`` message that actually moves a window, and the manager
    only honours that for a window it has *already* managed: sent any earlier
    it is dropped without a trace. So the request goes out once more, right
    after the map.

    This is not theory. With only the pre-map property, Muffin maps the overlay
    on the workspace winTab was started in, overwrites the property with that
    workspace's number, and never composites the strip anywhere else -- the
    gesture runs, the windows switch, and the screen shows nothing at all.
    """
    win.show_all()
    gdk_window.raise_()
    if make_sticky is not None:
        make_sticky(int(gdk_window.get_xid()))


# ---------------------------------------------------------------------------
# Layout: pure functions, so the geometry can be tested without a screen
# ---------------------------------------------------------------------------


def pitch_of(slot_w: float, gap: float) -> float:
    """Distance from one slot's left edge to the next."""
    return slot_w + gap


def visible_slots(screen_w: float, slot_w: float, gap: float) -> int:
    """How many whole slots fit across the screen."""
    if screen_w <= 0 or slot_w <= 0:
        return 1
    pitch = pitch_of(slot_w, gap)
    return max(1, int((screen_w + gap) // pitch))


def row_width(count: int, slot_w: float, gap: float) -> float:
    """Width of `count` slots in a row (no trailing gap)."""
    if count <= 0:
        return 0.0
    return count * slot_w + (count - 1) * gap


def view_target(selection: int, count: int, visible: int) -> float:
    """Index of the leftmost slot to draw, keeping the selection centred.

    Only moves when there are more windows than fit; when everything fits this
    is always 0 and the whole row is centred instead.
    """
    if count <= visible:
        return 0.0
    return float(max(0, min(count - visible, selection - (visible - 1) / 2.0)))


def row_origin(count: int, visible: int, screen_w: float, slot_w: float, gap: float,
               view: float) -> float:
    """Screen x of slot 0's left edge."""
    if count <= 0:
        return 0.0
    if count <= visible:
        return (screen_w - row_width(count, slot_w, gap)) / 2.0
    visible_width = row_width(visible, slot_w, gap)
    return (screen_w - visible_width) / 2.0 - view * pitch_of(slot_w, gap)


def slot_left(index: float, origin: float, pitch: float) -> float:
    """Screen x of slot `index`'s left edge (index may be fractional)."""
    return origin + index * pitch


def slot_centre_x(index: float, origin: float, pitch: float, slot_w: float) -> float:
    return slot_left(index, origin, pitch) + slot_w / 2.0


def thumbnail_resize(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Shrink a window crop to card size, quickly and without ringing.

    Pillow's LANCZOS on a big reduction is both slow -- 47ms for one
    full-screen window, which was the entire cost of preparing a strip -- and
    prone to ringing at this ratio. ``reduce`` halves with a box filter for
    almost nothing and is a better prefilter, so the final LANCZOS step has
    very little left to do (measured 11ms for the same window).
    """
    target_w, target_h = size
    if target_w <= 0 or target_h <= 0:
        return img
    current = img
    while (
        current.width >= 2 * target_w
        and current.height >= 2 * target_h
        and current.width > 16
        and current.height > 16
    ):
        current = current.reduce(2)
    if current.size == (target_w, target_h):
        return current
    return current.resize((target_w, target_h), Image.LANCZOS)


def card_size(win_w: float, win_h: float, slot_w: float, row_h: float) -> tuple[int, int]:
    """Size of a card inside its slot: aspect preserved, bounded by the slot.

    A tall or narrow window gets a narrow card centred in a normal slot, which
    is what keeps the row evenly spaced while every picture keeps its shape.
    """
    if win_w <= 0 or win_h <= 0 or slot_w <= 0 or row_h <= 0:
        return (max(1, int(slot_w)), max(1, int(row_h)))
    aspect = win_w / win_h
    height = min(row_h, slot_w / aspect)
    width = height * aspect
    return (max(1, int(round(width))), max(1, int(round(height))))


class _Buffer:
    """Keeps a pixel buffer alive for as long as its cairo surface exists.

    ``ImageSurface.create_for_data`` borrows the memory instead of copying it,
    so dropping the bytearray would leave cairo reading freed memory.
    """

    __slots__ = ("surface", "data")

    def __init__(self, img: Image.Image):
        self.data = bytearray(img.tobytes("raw", "BGRX"))
        # RGB24 is the native BGRX layout: no alpha channel, so no
        # premultiplication surprises, and it reads as opaque.
        self.surface = cairo.ImageSurface.create_for_data(
            self.data, cairo.FORMAT_RGB24, img.width, img.height, img.width * 4
        )


@dataclasses.dataclass
class Card:
    """One window's frozen picture, ready to blit."""

    key: str
    label: str
    xid: int
    size: tuple[int, int]
    padding: int
    surface: cairo.ImageSurface
    dim_surface: cairo.ImageSurface
    has_pixels: bool = True
    #: True when this card was cropped out of a screen grab, so it is only
    #: valid for the workspace that grab was taken on. A window's own pixels
    #: are valid anywhere.
    from_screen: bool = False
    buffers: tuple[_Buffer, ...] = ()

    @property
    def width(self) -> int:
        return self.size[0]

    @property
    def height(self) -> int:
        return self.size[1]


class Carousel:
    """Owns the overlay window, the card strip and the selection."""

    def __init__(self, config, make_sticky=None, clear_attention=None):
        self.config = config
        #: Called with the overlay's X window id as soon as the window exists,
        #: before it is ever mapped. The app uses it to ask the window manager
        #: to draw the strip on every workspace; None is for tests with no X.
        self._make_sticky = make_sticky
        #: Called with the overlay's X window id shortly after it is mapped
        #: (and again with every sticky re-assertion): the app withdraws the
        #: ``_NET_WM_STATE_DEMANDS_ATTENTION`` flag the manager stamps on a
        #: permanently-mapped notifications-type window, which otherwise makes
        #: its focus-stealing prevention route every newly opened application
        #: into "flash for attention" instead of raising it. None = tests.
        self._clear_attention_callback = clear_attention
        self.window: Gtk.Window | None = None

        # layout, recomputed whenever the strip opens
        self._width = 0
        self._height = 0
        self._slot_w = 0.0
        self._gap = 0.0
        self._pitch = 0.0
        self._visible = 1
        self._row_h = 0.0
        self._row_top = 0.0
        self._band_top = 0.0
        self._band_h = 0.0

        # the gesture
        self._cards: list[Card] = []
        self._start_index = 0
        self._selection = 0
        self._progress = 0.0
        self._state = IDLE
        self._tug = 0.0

        # animated selection outline: slot position and card size
        self._sel_float = 0.0
        self._outline_from = (0.0, 0.0, 0.0)
        self._outline_to = (0.0, 0.0, 0.0)
        self._outline_w = 0.0
        self._outline_h = 0.0
        self._view = 0.0
        self._view_from = 0.0
        self._view_to = 0.0
        self._slide_t0 = 0.0
        self._sliding = False

        # caches, all replaced wholesale so the GTK thread never sees a
        # half-built one
        self._backdrop: _Buffer | None = None
        self._backdrop_generation = -1
        self._cards_by_xid: dict[int, Card] = {}
        self._placeholders: dict[str, Card] = {}
        self._buffer_generation = -1

        # fade
        self._fade_from = 0.0
        self._fade_t0 = 0.0
        self._on_done = None

        # Withdrawing the attention flag the window manager stamps on the
        # overlay when it first takes it on (see the constant, below).
        self._attention_clears = 0

        self._sticky_tries = 0

        self._ticks = 0
        self._frames = 0
        self._frame_ms = 0.0
        self._frame_max_ms = 0.0
        self._last_frame_at = 0.0
        self._fps = 0.0
        self._draws = 0

    # -- window ------------------------------------------------------------
    def ensure_window(self) -> Gtk.Window:
        """Create (once) and map the overlay: invisible, click-through, on top."""
        if self.window is not None:
            return self.window

        screen = Gdk.Screen.get_default()
        if screen is None:
            raise RuntimeError("no Gdk screen: is DISPLAY set?")
        width = screen.get_width()
        height = screen.get_height()
        self._width, self._height = width, height
        self._layout()

        win = Gtk.Window(type=Gtk.WindowType.TOPLEVEL)
        win.set_decorated(False)
        win.set_resizable(False)
        win.set_skip_taskbar_hint(True)
        win.set_skip_pager_hint(True)
        win.set_accept_focus(False)
        win.set_focus_on_map(False)
        win.set_type_hint(Gdk.WindowTypeHint.NOTIFICATION)
        # NOTE: keep-above is deliberately NOT set here. Muffin stacks by layer
        # and keeps an ABOVE window at the very top of the stack it compares
        # against when a new window maps; a permanently above overlay made every
        # subsequently opened application "land behind" -- the manager denied it
        # focus (focus-stealing prevention) and hung the attention flag on it
        # instead. The strip's visibility is a gesture's lifetime, so `open()`
        # pins ABOVE (via _keep_on_top) and the close paths hand the layer back
        # (see _release_on_top).
        visual = screen.get_rgba_visual()
        if visual is not None:
            win.set_visual(visual)
        win.set_app_paintable(True)
        win.set_default_size(width, height)
        win.move(0, 0)
        win.set_opacity(0.0)  # no flash while it is being set up
        win.connect("draw", self._on_draw)
        win.connect("destroy", self._on_destroy)

        gdk_window = realize_overlay(win, self._make_sticky)
        # Empty input shape: nothing the overlay draws can be clicked.
        gdk_window.input_shape_combine_region(cairo.Region(), 0, 0)
        map_overlay(win, gdk_window, self._make_sticky)
        self._retry_sticky_soon()
        self._clear_attention_soon()
        self.window = win
        return win

    def close(self) -> None:
        if self.window is not None:
            self._stop_ticks()
            window, self.window = self.window, None
            window.destroy()

    def _on_destroy(self, *_args) -> None:
        self.window = None
        self._state = IDLE

    def _layout(self) -> None:
        """Work out the row geometry for the current screen."""
        width, height = self._width, self._height
        self._slot_w = width * self.config.slot_fraction
        self._gap = width * self.config.card_gap_fraction
        self._pitch = pitch_of(self._slot_w, self._gap)
        self._visible = visible_slots(width, self._slot_w, self._gap)
        self._row_h = height * self.config.max_card_height_fraction
        self._row_top = (height - self._row_h) / 2.0
        self._band_top = self._row_top - self.config.band_pad_px
        self._band_h = self._row_h + 2.0 * self.config.band_pad_px

    # -- pixels (called from the capture thread) ---------------------------
    def prewarm(self, plain: Image.Image | None, backdrop: Image.Image | None,
                generation: int, window_rects: dict,
                window_pixels: dict | None = None,
                allow_crop: bool = True, cancel=None) -> None:
        """Turn a fresh screen snapshot into ready-to-blit cards.

        ``plain`` is the raw screen (the fallback source of a window's pixels)
        and ``backdrop`` is the blurred, dimmed version of it. ``window_pixels``
        holds each window's *own* pixels, read from the compositor, which is
        what a card should show: cropping the screen at a window's rect also
        picks up whatever happens to be in front of it. Windows with no own
        pixels fall back to that crop.        Runs on the capture worker thread: one
        crop, one scale and two small surfaces per window, never while a
        gesture is on screen. The results are swapped in at the end so the GTK
        thread only ever sees a complete set.

        ``allow_crop=False`` is for a screen grab that was taken on a different
        workspace than the windows came from: then the crop is not this window
        at all, and the window waits for a blank card instead (its name, no
        picture) until the next grab lands -- a fraction of a second.

        ``cancel`` is called between windows: when a swipe starts while this is
        running, the remaining cards can wait. Nothing is swapped in that case,
        so the strip keeps the last complete set of cards instead of showing a
        half-updated row.
        """
        if self._width <= 0 or self._height <= 0:
            return  # the window has not been created yet
        prepared_backdrop = self._build_backdrop(backdrop or plain)
        own = window_pixels or {}

        cards: dict[int, Card] = {}
        for xid, rect in window_rects.items():
            if cancel is not None and cancel():
                return
            source = own.get(int(xid))
            cropped = source is None
            if cropped:
                if not allow_crop:
                    continue
                source = self._crop(plain, rect) if plain is not None else None
            if source is None:
                continue
            try:
                cards[int(xid)] = self._make_card(
                    int(xid), str(xid), rect, source, from_screen=cropped
                )
            except (ValueError, OSError, cairo.Error):
                continue

        for xid, picture in own.items():
            if cancel is not None and cancel():
                return
            # A window we have pixels for but no rect for (it moved between the
            # grab and the window list) still deserves a card.
            if int(xid) not in cards:
                try:
                    cards[int(xid)] = self._make_card(
                        int(xid), str(xid), (0, 0, picture.width, picture.height), picture
                    )
                except (ValueError, OSError, cairo.Error):
                    continue

        self._backdrop = prepared_backdrop
        self._backdrop_generation = generation
        self._cards_by_xid = cards
        self._placeholders = {}
        self._buffer_generation = generation

    def _build_backdrop(self, plain: Image.Image | None) -> _Buffer | None:
        if plain is None:
            return None
        try:
            return _Buffer(plain.convert("RGB"))
        except (ValueError, OSError):
            return None

    @staticmethod
    def _crop(plain: Image.Image, rect: tuple[int, int, int, int]) -> Image.Image | None:
        x, y, w, h = (int(v) for v in rect)
        if w <= 8 or h <= 8:
            return None
        sw, sh = plain.size
        left, top = max(0, x), max(0, y)
        right, bottom = min(sw, x + w), min(sh, y + h)
        if right - left < 8 or bottom - top < 8:
            return None
        return plain.crop((left, top, right, bottom))

    def _make_card(self, xid: int, key: str, rect, crop: Image.Image | None,
                   label: str = "", from_screen: bool = False) -> Card:
        """Bake one window into a bright card and a dimmed twin.

        The picture's own size wins over ``rect``: a window's off-screen pixels
        include its decoration, so they are the true shape of the thing.
        """
        if crop is not None and crop.width > 0 and crop.height > 0:
            width, height = crop.width, crop.height
        else:
            width, height = (int(v) for v in rect[2:4])
        card_w, card_h = card_size(width, height, self._slot_w, self._row_h)
        padding = int(self.config.card_shadow_px) + 2

        body: Image.Image | None = None
        if crop is not None:
            body = thumbnail_resize(crop.convert("RGB"), (card_w, card_h))
        dim_body = None
        if body is not None and self.config.dim_others > 0:
            black = Image.new("RGB", (card_w, card_h), (0, 0, 0))
            dim_body = Image.blend(body, black, min(1.0, max(0.0, self.config.dim_others)))

        bright, bright_buffers = self._compose_card(body, card_w, card_h, padding, 0.0)
        if dim_body is None:
            dim, dim_buffers = bright, ()
        else:
            dim, dim_buffers = self._compose_card(dim_body, card_w, card_h, padding, 0.35)

        return Card(
            key=key,
            label=label,
            xid=xid,
            size=(card_w, card_h),
            padding=padding,
            surface=bright,
            dim_surface=dim,
            has_pixels=body is not None,
            from_screen=from_screen,
            buffers=tuple(bright_buffers) + tuple(dim_buffers),
        )

    def _compose_card(self, body: Image.Image | None, card_w: int, card_h: int,
                      padding: int, placeholder_bias: float):
        """A card surface: soft shadow, rounded body, optional picture."""
        surface = cairo.ImageSurface(
            cairo.FORMAT_ARGB32, card_w + 2 * padding, card_h + 2 * padding
        )
        ctx = cairo.Context(surface)
        self._paint_shadow(ctx, padding, padding, card_w, card_h)

        buffers: list[_Buffer] = []
        ctx.save()
        _rounded_rect(ctx, padding, padding, card_w, card_h, self.config.card_radius)
        ctx.clip()
        if body is None:
            shade = min(0.5, 0.12 + placeholder_bias)
            ctx.set_source_rgba(shade + 0.06, shade + 0.07, shade + 0.10, 1.0)
            ctx.rectangle(padding, padding, card_w, card_h)
            ctx.fill()
        else:
            buffer = _Buffer(body)
            buffers.append(buffer)
            ctx.set_source_surface(buffer.surface, padding, padding)
            ctx.paint()
        ctx.restore()

        if body is None:
            # A window with no pixels available (minimized, or captured before
            # it existed) still gets a card, so the strip never jumps.
            ctx.save()
            _rounded_rect(ctx, padding + 1, padding + 1, card_w - 2, card_h - 2,
                          self.config.card_radius)
            ctx.set_source_rgba(1.0, 1.0, 1.0, 0.10 - placeholder_bias * 0.2)
            ctx.set_line_width(2.0)
            ctx.stroke()
            ctx.restore()
        return surface, buffers

    def _paint_shadow(self, ctx, x, y, w, h) -> None:
        """A soft shadow under a card, drawn as a few cheap rounded layers."""
        spread = float(self.config.card_shadow_px)
        if spread <= 0:
            return
        layers = max(3, int(spread // 9))
        ctx.save()
        for layer in range(layers, 0, -1):
            grow = spread * (layer / layers)
            ctx.set_source_rgba(0.0, 0.0, 0.0, 0.05)
            _rounded_rect(ctx, x - grow, y - grow + grow * 0.30, w + grow * 2.0,
                          h + grow * 2.0, self.config.card_radius + grow)
            ctx.fill()
        ctx.restore()

    @property
    def generation(self) -> int:
        return self._buffer_generation

    # -- gesture API (GTK thread) ------------------------------------------
    @property
    def visible(self) -> bool:
        return self._state != IDLE

    @property
    def state(self) -> str:
        return self._state

    @property
    def progress(self) -> float:
        return self._progress

    @property
    def selection(self) -> int:
        return self._selection

    @property
    def count(self) -> int:
        return len(self._cards)

    @property
    def current(self) -> Card | None:
        if 0 <= self._selection < len(self._cards):
            return self._cards[self._selection]
        return None

    def open(self, entries, index: int) -> None:
        """Lay out the strip and bring it up with `index` outlined."""
        self.ensure_window()
        self._layout()

        cards: list[Card] = []
        for entry in entries:
            card = self._card_for(entry)
            if card is not None:
                cards.append(card)
        self._cards = cards
        if not cards:
            self._state = IDLE
            return

        self._start_index = max(0, min(len(cards) - 1, int(index)))
        self._selection = self._start_index
        self._progress = 0.0
        self._tug = 0.0
        self._sel_float = float(self._selection)
        card = cards[self._selection]
        self._outline_w, self._outline_h = float(card.width), float(card.height)
        self._outline_from = self._outline_to = self._snapshot_outline()
        self._view = self._view_from = self._view_to = view_target(
            self._selection, len(cards), self._visible
        )
        self._sliding = False
        self._frame_max_ms = 0.0
        self._state = DRAG
        window = self._window()
        window.set_opacity(1.0)
        self._keep_on_top()
        # A manager that (re)appeared while winTab was running -- or one that
        # only now got around to managing the overlay -- stamps the attention
        # flag on when it takes the window on. The flag makes every *next*
        # natural window-open "land behind", so it is withdrawn immediately
        # instead of waiting for the map-time retries.
        self._clear_attention_soon()
        window.queue_draw()

    def forget_screen_cards(self) -> None:
        """Drop every card taken from a screen grab, after a workspace switch.

        Those cards are the previous workspace's desktop cropped at some rect:
        for a window that lives on both workspaces (a sticky one) the card would
        otherwise be reused and show the wrong thing. Cards built from a
        window's own pixels are valid anywhere and stay.
        """
        self._cards_by_xid = {
            xid: card for xid, card in self._cards_by_xid.items() if not card.from_screen
        }
        self._placeholders = {}

    def _card_for(self, entry) -> Card | None:
        """The cached card for an entry, or a placeholder for this gesture."""
        for ref in entry.windows:
            card = self._cards_by_xid.get(ref.xid)
            if card is not None:
                # The entry is the one source of names, so a prewarmed card and
                # a placeholder are labelled the same way.
                card.label = entry.label
                return card
        top = entry.top
        placeholder = self._placeholders.get(entry.key)
        if placeholder is None:
            try:
                placeholder = self._make_card(top.xid, entry.key, top.rect, None, entry.label)
            except (ValueError, OSError, cairo.Error):
                return None
            self._placeholders[entry.key] = placeholder
        return placeholder

    def set_progress(self, progress: float, steps: int | None = None) -> None:
        """Follow the fingers: raw progress in window steps, overshoot included.

        ``steps`` is the window the engine says this progress points at; when it
        is absent (a scripted carousel) the same rule is applied locally.
        """
        if self._state != DRAG or not self._cards:
            return
        self._progress = progress
        if steps is None:
            steps = highlight_index(progress, self._start_index, len(self._cards)) - self._start_index
        self._move_selection(self._start_index + steps)

        tug = self._compute_tug()
        if abs(tug - self._tug) * self._pitch >= 0.25:
            self._tug = tug
            self._window().queue_draw()

    def settle(self, steps: int, on_commit=None, on_done=None) -> None:
        """Open the outlined window, then fade out.

        There is nothing to animate first: the outline has been showing the
        target for the whole drag, so the window is activated immediately and
        the strip fades over it.
        """
        self._on_done = on_done
        if self._state == IDLE:
            if on_commit:
                on_commit()
            self._finish_fade()
            return
        if steps:
            if on_commit:
                try:
                    on_commit()
                except Exception as exc:  # a failed activation must not strand us
                    print(f"wintab: commit failed: {exc}")
        self._begin_fade()

    def cancel(self, on_done=None) -> None:
        """Fade out without switching: nothing was dragged far enough."""
        self._on_done = on_done
        if self._state == IDLE:
            self._finish_fade()
            return
        self._begin_fade()

    def abort(self, on_done=None) -> None:
        """Drop the strip immediately (device lost, shutdown)."""
        self._on_done = None
        self._sliding = False
        self._progress = 0.0
        self._state = IDLE
        self._cards = []
        self._stop_ticks()
        if self.window is not None:
            self.window.set_opacity(0.0)
            self._release_on_top()
            self.window.queue_draw()
        if on_done:
            on_done()

    # -- selection ---------------------------------------------------------
    def _move_selection(self, selection: int) -> None:
        if not self._cards:
            return
        selection = max(0, min(len(self._cards) - 1, int(selection)))
        if selection == self._selection and self._sliding:
            return
        if selection == self._selection:
            return
        previous = self._cards[self._selection]
        target = self._cards[selection]
        self._selection = selection
        self._outline_from = (self._sel_float, float(previous.width), float(previous.height))
        self._outline_to = (float(selection), float(target.width), float(target.height))
        self._view_from = self._view
        self._view_to = view_target(selection, len(self._cards), self._visible)
        self._slide_t0 = time.monotonic()
        self._sliding = True
        self._start_ticks()

    def _snapshot_outline(self) -> tuple[float, float, float]:
        return (self._sel_float, self._outline_w, self._outline_h)

    def _compute_tug(self) -> float:
        """A small sideways tug of the whole row, in slot units."""
        parallax = self.config.row_parallax
        if not parallax or not self._cards:
            return 0.0
        tugged = apply_rubber_band(
            self._progress * parallax, self._start_index, len(self._cards),
            self.config.rubber_band,
        )
        return max(-0.5, min(0.5, tugged))

    def _row_origin(self) -> float:
        origin = row_origin(
            len(self._cards), self._visible, self._width, self._slot_w, self._gap, self._view
        )
        return origin + self._tug * self._pitch

    def _card_position(self, index: float, card: Card) -> tuple[float, float]:
        centre = slot_centre_x(index, self._row_origin(), self._pitch, self._slot_w)
        return (centre - card.width / 2.0, self._row_top + (self._row_h - card.height) / 2.0)

    def card_rects(self) -> list[tuple[float, float, float, float]]:
        """Where each card lands on screen, for tests and the visual spike."""
        rects = []
        for index, card in enumerate(self._cards):
            x, y = self._card_position(float(index), card)
            rects.append((x, y, float(card.width), float(card.height)))
        return rects

    def outline_rect(self) -> tuple[float, float, float, float]:
        """The selection outline, including its gap, in screen coordinates."""
        gap = self.config.highlight_gap_px
        card = self._cards[self._selection] if self._cards else None
        width = self._outline_w if card is None else self._outline_w
        height = self._outline_h if card is None else self._outline_h
        centre = slot_centre_x(self._sel_float, self._row_origin(), self._pitch, self._slot_w)
        x = centre - width / 2.0
        y = self._row_top + (self._row_h - height) / 2.0
        return (x - gap, y - gap, width + 2 * gap, height + 2 * gap)

    # -- animation ---------------------------------------------------------
    def _window(self) -> Gtk.Window:
        return self.window if self.window is not None else self.ensure_window()

    def _assert_sticky(self) -> None:
        """Tell the window manager (again) that the overlay is everywhere."""
        if self._make_sticky is None or self.window is None:
            return
        gdk_window = self.window.get_window()
        if gdk_window is not None:
            self._make_sticky(int(gdk_window.get_xid()))

    def _retry_sticky_soon(self) -> None:
        """Repeat the sticky request until the manager has taken the window on.

        The request cannot be made at map time: a window manager only accepts a
        state change for a window it already manages, and the map is still in
        flight while the window is being shown. Measured: without these retries
        Muffin maps the overlay on the workspace winTab was started in, writes
        that workspace's number into ``_NET_WM_DESKTOP``, and never composites
        the strip anywhere else -- the gesture keeps working, so it looks like
        the strip simply never opens.
        """
        if self._make_sticky is None or self._sticky_tries >= STICKY_RETRIES:
            return
        GLib.timeout_add(STICKY_RETRY_MS, self._retry_sticky)

    def _retry_sticky(self) -> bool:
        if self._sticky_tries >= STICKY_RETRIES:
            return False
        self._sticky_tries += 1
        self._assert_sticky()
        # A sticky re-assertion re-runs the state machinery that stamps the
        # attention flag on, so the map-time "clear" above has to be repeated
        # alongside it -- the two requests ride together for the window's
        # whole life.
        self._clear_attention_soon()
        return self._sticky_tries < STICKY_RETRIES

    # -- the attention flag --------------------------------------------------
    def _clear_attention_soon(self) -> None:
        """Ask the manager to hand the attention flag back, shortly from now.

        Right after the map the flag is already standing (observed on this
        session's running instance), and it is re-set by every later sticky
        request. The removal message is only honoured for a window the manager
        has taken on entirely, which may be a beat behind the map -- hence the
        retry loop rather than a single, optimistic call.
        """
        GLib.timeout_add(ATTENTION_CLEAR_DELAY_MS, self._clear_attention_retry)

    def _clear_attention_retry(self) -> bool:
        if self._attention_clears >= ATTENTION_CLEAR_RETRIES:
            return False  # capped, like the sticky retries
        if self.window is None:
            return False  # overlay closed: nothing to clear
        gdk_window = self.window.get_window()
        if gdk_window is not None:
            self._clear_attention(int(gdk_window.get_xid()))
        self._attention_clears += 1
        return self._attention_clears < ATTENTION_CLEAR_RETRIES

    def _clear_attention(self, xid: int) -> None:
        """Withdraw DEMANDS_ATTENTION (the app supplies the actual request)."""
        if self._clear_attention_callback is not None:
            self._clear_attention_callback(int(xid))

    def _keep_on_top(self) -> None:
        """Pin ABOVE for the life of a gesture and raise the overlay.

        Nothing unmaps the window, but every workspace brings its own windows
        and moving to one restacks them; raising before each open costs a single
        XConfigureWindow and is the difference between a visible strip and a
        silent switch. The sticky request goes out with it, which is also the
        answer to a compositor that has been restarted since we last spoke.

        The ABOVE state is set here rather than once at map time on purpose:
        while it stands between gestures, Muffin keeps this full-screen window
        at the head of the stack it consults when a *new* application window
        maps, and focus-stealing prevention then denies that window focus --
        it opens behind and heads up flashing (measured: a launched calculator
        kept ``_NET_WM_STATE_DEMANDS_ATTENTION`` indefinitely with ABOVE
        standing, and focused in 0.1s with it gone). With fingers down nothing
        else can appear, so the layer only has to cover the gesture.
        """
        self._assert_sticky()
        if self.window is None:
            return
        gdk_window = self.window.get_window()
        if gdk_window is not None:
            gdk_window.set_keep_above(True)
            gdk_window.raise_()

    def _release_on_top(self) -> None:
        """Hand the above-layer back once the strip is gone.

        The mirror of ``_keep_on_top``: called when the strip finishes fading
        (or is aborted), so between gestures the overlay is an ordinary window
        that no longer sits at the head of Muffin's focus-decision stack.
        GTK applies keep-above via two X calls (the state message and
        XConfigureWindow); undoing both matches what the setter did.
        """
        if self.window is None:
            return
        gdk_window = self.window.get_window()
        if gdk_window is not None:
            gdk_window.set_keep_above(False)

    def _begin_fade(self) -> None:
        self._fade_from = 1.0
        self._fade_t0 = time.monotonic()
        self._state = FADE
        self._start_ticks()

    def _start_ticks(self) -> None:
        if self._ticks:
            return
        self._ticks = self._window().add_tick_callback(self._on_tick)

    def _stop_ticks(self) -> None:
        if not self._ticks:
            return
        tick, self._ticks = self._ticks, 0
        try:
            if self.window is not None and self.window.get_realized():
                self.window.remove_tick_callback(tick)
        except Exception:
            pass

    def _advance_slide(self, now: float) -> bool:
        """Move the outline (and the row, if it has to scroll) toward its target."""
        if not self._sliding:
            return False
        total = max(1e-4, self.config.highlight_ms / 1000.0)
        t = (now - self._slide_t0) / total
        eased = ease_out(t)
        self._sel_float = lerp(self._outline_from[0], self._outline_to[0], eased)
        self._outline_w = lerp(self._outline_from[1], self._outline_to[1], eased)
        self._outline_h = lerp(self._outline_from[2], self._outline_to[2], eased)
        self._view = lerp(self._view_from, self._view_to, eased)
        if t >= 1.0:
            self._sel_float = self._outline_to[0]
            self._outline_w, self._outline_h = self._outline_to[1], self._outline_to[2]
            self._view = self._view_to
            self._sliding = False
        return True

    def _on_tick(self, _widget, _clock) -> bool:
        now = time.monotonic()
        dirty = self._advance_slide(now)

        if self._state == FADE:
            total = max(1e-4, self.config.fade_ms / 1000.0)
            t = (now - self._fade_t0) / total
            if self.window is not None:
                self.window.set_opacity(max(0.0, self._fade_from * (1.0 - min(1.0, t))))
            dirty = True
            if t >= 1.0:
                self._finish_fade()
                return False

        if dirty and self.window is not None:
            self.window.queue_draw()
        if not self._sliding and self._state != FADE:
            self._stop_ticks()
            return False
        return True

    def _finish_fade(self) -> None:
        self._state = IDLE
        self._sliding = False
        self._cards = []
        if self.window is not None:
            self.window.set_opacity(0.0)
            # The strip is gone: give the above-layer back. Keeping it would
            # pin this full-screen window at the head of Muffin's focus-decision
            # stack and make every newly opened application "open behind"
            # (see _keep_on_top for the measured failure).
            self._release_on_top()
            self.window.queue_draw()
        self._stop_ticks()
        callback, self._on_done = self._on_done, None
        if callback:
            try:
                callback()
            except Exception as exc:
                print(f"wintab: overlay callback failed: {exc}")

    # -- drawing -----------------------------------------------------------
    def _on_draw(self, _widget, ctx) -> bool:
        started = time.monotonic()
        ctx.save()
        ctx.set_operator(cairo.OPERATOR_SOURCE)
        ctx.set_source_rgba(0, 0, 0, 0)
        ctx.paint()
        ctx.set_operator(cairo.OPERATOR_OVER)
        try:
            self._render(ctx)
        finally:
            ctx.restore()
        self._track_fps(started)
        return False

    def _render(self, ctx) -> None:
        width, height = self._width, self._height
        if self._state == IDLE or not self._cards or width <= 0:
            return

        if self._backdrop is not None:
            ctx.set_source_surface(self._backdrop.surface, 0, 0)
            ctx.paint()
        else:
            ctx.set_source_rgb(0.06, 0.06, 0.08)
            ctx.paint()

        # The lighter band Windows 11 puts behind the row.
        if self.config.band_alpha > 0:
            ctx.set_source_rgba(1.0, 1.0, 1.0, self.config.band_alpha)
            ctx.rectangle(0, self._band_top, width, self._band_h)
            ctx.fill()

        for index, card in enumerate(self._cards):
            x, y = self._card_position(float(index), card)
            if x + card.width < -card.padding or x - card.padding > width:
                continue  # scrolled off: skip the blit entirely
            surface = card.surface if index == self._selection else card.dim_surface
            ctx.set_source_surface(surface, x - card.padding, y - card.padding)
            ctx.paint()
            self._draw_caption(ctx, card, x, y)

        self._draw_outline(ctx)

        if self.config.show_fps:
            self._draw_hud(ctx)

    def _draw_caption(self, ctx, card: Card, x: float, y: float) -> None:
        """The application's name under its card, like Windows 11 labels one.

        Drawn per frame rather than baked into the card: it costs a fraction of
        a millisecond, and it means the label always comes from the same place
        the rest of the strip gets it from (the entry), so a window that
        changes identity between gestures cannot show a stale name.
        """
        font_px = self.config.caption_font_px
        if font_px <= 0 or not card.label:
            return
        ctx.save()
        # Clip to the card's own column: a very long name gets cut off instead
        # of running into its neighbour.
        ctx.rectangle(x - card.padding, y, card.width + 2 * card.padding, card.height + font_px * 2)
        ctx.clip()
        ctx.select_font_face("sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        ctx.set_font_size(font_px)
        extents = ctx.text_extents(card.label)
        left = x + (card.width - extents.width) / 2.0 - extents.x_bearing
        baseline = caption_baseline(y, card.height, self.config.caption_gap_px, font_px)
        ctx.move_to(left + 1.0, baseline + 1.0)
        ctx.set_source_rgba(0.0, 0.0, 0.0, 0.6)
        ctx.show_text(card.label)
        ctx.move_to(left, baseline)
        ctx.set_source_rgba(1.0, 1.0, 1.0, 0.92)
        ctx.show_text(card.label)
        ctx.restore()

    def _draw_outline(self, ctx) -> None:
        x, y, w, h = self.outline_rect()
        radius = self.config.card_radius + self.config.highlight_gap_px
        colour = self.config.highlight_color
        ctx.save()
        # A faint white rim just outside, then the accent blue, like Windows.
        _rounded_rect(ctx, x - 1.5, y - 1.5, w + 3.0, h + 3.0, radius + 1.5)
        ctx.set_source_rgba(1.0, 1.0, 1.0, 0.45)
        ctx.set_line_width(1.0)
        ctx.stroke()
        _rounded_rect(ctx, x, y, w, h, radius)
        ctx.set_source_rgb(*colour)
        ctx.set_line_width(self.config.highlight_px)
        ctx.stroke()
        ctx.restore()

    def _draw_hud(self, ctx) -> None:
        ctx.save()
        ctx.select_font_face("monospace", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        ctx.set_font_size(18.0)
        ctx.set_source_rgba(0.0, 0.0, 0.0, 0.6)
        ctx.rectangle(12, 12, 380, 34)
        ctx.fill()
        ctx.set_source_rgba(1.0, 1.0, 1.0, 0.95)
        ctx.move_to(22, 36)
        ctx.show_text(
            f"wintab {self._state} sel={self._selection}/{len(self._cards)} "
            f"p={self._progress:+.2f} {self._fps:4.0f}fps {self._frame_ms:4.1f}ms"
        )
        ctx.restore()

    def _track_fps(self, started: float) -> None:
        self._frame_ms = (time.monotonic() - started) * 1000.0
        self._frame_max_ms = max(self._frame_max_ms, self._frame_ms)
        self._frames += 1
        self._draws += 1
        now = time.monotonic()
        if not self._last_frame_at:
            self._last_frame_at = now
        elif now - self._last_frame_at >= 0.5:
            self._fps = self._frames / (now - self._last_frame_at)
            self._frames = 0
            self._last_frame_at = now

    @property
    def stats(self) -> dict:
        return {
            "state": self._state,
            "cards": len(self._cards),
            "selection": self._selection,
            "view": round(self._view, 3),
            "progress": round(self._progress, 3),
            "frame_ms": round(self._frame_ms, 1),
            "frame_max_ms": round(self._frame_max_ms, 1),
            "fps": round(self._fps, 1),
            "draws": self._draws,
            "backdrop": self._backdrop is not None,
        }
