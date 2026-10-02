"""Tests for the card strip geometry, which needs no screen.

The window, the pixels and the animation need a live X server (see
``tools/spike_carousel.py``), but the layout -- how wide a card is, where a slot
lands, where the row scrolls to -- is pure arithmetic, and it is the part that
is easy to get subtly wrong.
"""

from __future__ import annotations

import unittest

from PIL import Image

from wintab import overlay
from wintab.config import Config

SCREEN = (1920, 1080)


def defaults():
    return Config()


class CardPixelsTests(unittest.TestCase):
    """A card must show the window, not whatever happened to be on top of it.

    Baking happens on the capture thread and needs no GTK window, so this can
    be checked here: give the carousel a red screen and a green window and see
    which colour ends up in the middle of the card.
    """

    def _carousel(self):
        strip = overlay.Carousel(defaults())
        strip._width, strip._height = 1280, 800
        strip._layout()
        return strip

    def _middle_pixel(self, card: overlay.Card) -> tuple[int, int, int]:
        # The card's surface is drawn from its own buffer, B,G,R,X per pixel.
        data = card.buffers[0].data
        width = card.width + 2 * card.padding
        index = ((card.padding + card.height // 2) * width + card.padding + card.width // 2) * 4
        return (data[index + 2], data[index + 1], data[index])  # R, G, B

    def test_a_window_with_its_own_pixels_ignores_the_screen(self):
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        window = Image.new("RGB", (300, 200), (0, 255, 0))
        strip = self._carousel()
        strip.prewarm(screen, screen, 1, {7: (10, 10, 300, 200)}, {7: window})
        card = strip._cards_by_xid[7]
        self.assertTrue(card.has_pixels)
        self.assertEqual(card.size, overlay.card_size(300, 200, strip._slot_w, strip._row_h))
        red, green, blue = self._middle_pixel(card)
        self.assertGreater(green, 200, "the card should show the window, not the screen")
        self.assertLess(red, 60)
        self.assertLess(blue, 60)

    def test_without_own_pixels_the_screen_is_cropped_instead(self):
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        window = Image.new("RGB", (300, 200), (0, 255, 0))
        strip = self._carousel()
        # No own pixels for this window: the fallback crops the screen at its
        # rect, so the card comes out red even though the window is green.
        strip.prewarm(screen, screen, 1, {7: (10, 10, 300, 200)}, {})
        red, green, blue = self._middle_pixel(strip._cards_by_xid[7])
        self.assertGreater(red, 200)
        self.assertLess(green, 60)

    def test_a_window_with_pixels_but_no_rect_still_gets_a_card(self):
        # Windows move between the screen grab and the window list; the pixels
        # are still perfectly good.
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        window = Image.new("RGB", (300, 200), (0, 255, 0))
        strip = self._carousel()
        strip.prewarm(screen, screen, 1, {}, {7: window})
        self.assertEqual(sorted(strip._cards_by_xid), [7])
        red, green, _ = self._middle_pixel(strip._cards_by_xid[7])
        self.assertGreater(green, 200)
        self.assertLess(red, 60)

    def test_a_screen_crop_is_refused_for_another_workspace(self):
        # The grab was taken on the workspace we just left, where this window
        # is not visible: the crop would be somebody else's pixels (or the
        # wallpaper), so the window waits for a blank card instead.
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        strip = self._carousel()
        strip.prewarm(screen, screen, 1, {7: (10, 10, 300, 200)}, {},
                      allow_crop=False)
        self.assertEqual(strip._cards_by_xid, {})

    def test_own_pixels_survive_a_workspace_switch(self):
        # A window's own pixels are valid on any workspace, so they are still
        # used when the screen grab has to be distrusted.
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        window = Image.new("RGB", (300, 200), (0, 255, 0))
        strip = self._carousel()
        strip.prewarm(screen, screen, 1, {7: (10, 10, 300, 200)}, {7: window},
                      allow_crop=False)
        self.assertEqual(sorted(strip._cards_by_xid), [7])
        red, green, _ = self._middle_pixel(strip._cards_by_xid[7])
        self.assertGreater(green, 200)
        self.assertLess(red, 60)

    def test_forgetting_screen_cards_keeps_the_windows_own_ones(self):
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        window = Image.new("RGB", (300, 200), (0, 255, 0))
        strip = self._carousel()
        # 7 has its own pixels, 8 was cropped out of the screen.
        strip.prewarm(screen, screen, 1,
                      {7: (10, 10, 300, 200), 8: (400, 10, 300, 200)}, {7: window})
        self.assertEqual(sorted(strip._cards_by_xid), [7, 8])
        strip.forget_screen_cards()
        self.assertEqual(sorted(strip._cards_by_xid), [7])

    def test_no_pixels_means_no_cards_so_a_placeholder_steps_in(self):
        strip = self._carousel()
        strip.prewarm(None, None, 1, {}, {})
        self.assertEqual(strip._cards_by_xid, {})

    def test_a_frame_smaller_than_the_rect_shrinks_the_card(self):
        # A window's own pixels are the truth about its shape; the rect is only
        # a fallback. A card that trusted the rect here would be stretched.
        screen = Image.new("RGB", (1280, 800), (255, 0, 0))
        stripped = Image.new("RGB", (400, 100), (0, 255, 0))  # a wide, short frame
        strip = self._carousel()
        strip.prewarm(screen, screen, 1, {7: (10, 10, 400, 400)}, {7: stripped})
        card = strip._cards_by_xid[7]
        self.assertLess(card.height, card.width / 2,
                        "the card must keep the window's own shape")


class CaptionTests(unittest.TestCase):
    def test_the_name_sits_under_the_card(self):
        baseline = overlay.caption_baseline(100.0, 200.0, 6.0, 13.0)
        self.assertGreater(baseline, 300.0, "the name must clear the card")
        self.assertLess(baseline, 320.0, "...and stay inside the band under it")

    def test_a_bigger_font_sits_lower_not_higher(self):
        small = overlay.caption_baseline(0.0, 100.0, 6.0, 10.0)
        large = overlay.caption_baseline(0.0, 100.0, 6.0, 20.0)
        self.assertGreater(large, small)


class SlotMathTests(unittest.TestCase):
    def test_four_slots_fit_a_full_hd_screen(self):
        cfg = defaults()
        slot = SCREEN[0] * cfg.slot_fraction
        gap = SCREEN[0] * cfg.card_gap_fraction
        self.assertEqual(overlay.visible_slots(SCREEN[0], slot, gap), 4)

    def test_visible_slots_never_reports_zero(self):
        self.assertEqual(overlay.visible_slots(10, 400, 40), 1)
        self.assertEqual(overlay.visible_slots(0, 400, 40), 1)
        self.assertEqual(overlay.visible_slots(1920, 0, 40), 1)

    def test_row_width_has_no_trailing_gap(self):
        self.assertAlmostEqual(overlay.row_width(3, 400, 40), 3 * 400 + 2 * 40)
        self.assertEqual(overlay.row_width(1, 400, 40), 400)
        self.assertEqual(overlay.row_width(0, 400, 40), 0)

    def test_slots_are_one_pitch_apart(self):
        self.assertAlmostEqual(overlay.slot_left(0, 100.0, 456.0), 100.0)
        self.assertAlmostEqual(overlay.slot_left(3, 100.0, 456.0), 1468.0)
        self.assertAlmostEqual(
            overlay.slot_centre_x(2, 100.0, 456.0, 400.0), 100.0 + 2 * 456.0 + 200.0
        )


class ViewTests(unittest.TestCase):
    def test_no_scrolling_when_everything_fits(self):
        self.assertEqual(overlay.view_target(0, 3, 4), 0.0)
        self.assertEqual(overlay.view_target(2, 3, 4), 0.0)

    def test_selection_is_centred_when_the_row_can_scroll(self):
        # 8 windows, 4 slots: slot 5 sits centred with 1.5 slots to its left.
        self.assertAlmostEqual(overlay.view_target(5, 8, 4), 3.5)

    def test_scrolling_stops_at_the_ends(self):
        self.assertEqual(overlay.view_target(0, 8, 4), 0.0)
        self.assertEqual(overlay.view_target(1, 8, 4), 0.0)
        self.assertEqual(overlay.view_target(7, 8, 4), 4.0)
        self.assertEqual(overlay.view_target(6, 8, 4), 4.0)

    def test_row_is_centred_when_it_fits(self):
        left = overlay.row_origin(3, 4, 1920, 400, 40, 0.0)
        right = left + overlay.row_width(3, 400, 40)
        self.assertAlmostEqual(left, 1920 - right)

    def test_row_is_centred_on_the_visible_window_of_slots(self):
        visible_width = overlay.row_width(4, 400, 40)
        origin = overlay.row_origin(8, 4, 1920, 400, 40, 0.0)
        self.assertAlmostEqual(origin, (1920 - visible_width) / 2.0)
        scrolled = overlay.row_origin(8, 4, 1920, 400, 40, 2.0)
        self.assertAlmostEqual(scrolled, origin - 2.0 * overlay.pitch_of(400, 40))


class CardSizeTests(unittest.TestCase):
    def test_landscape_window_fills_its_slot_width(self):
        width, height = overlay.card_size(1440, 900, 422.0, 432.0)
        self.assertEqual(width, 422)
        self.assertAlmostEqual(width / height, 1440 / 900, places=2)

    def test_tall_window_is_narrow_inside_a_normal_slot(self):
        width, height = overlay.card_size(400, 900, 422.0, 432.0)
        self.assertEqual(height, 432)
        self.assertLess(width, 422, "a portrait window must not stretch to fill its slot")
        self.assertAlmostEqual(width / height, 400 / 900, places=2)

    def test_card_never_exceeds_its_slot(self):
        for window in [(100, 100), (1920, 1080), (300, 2000), (4000, 300), (16, 9)]:
            width, height = overlay.card_size(window[0], window[1], 422.0, 432.0)
            self.assertLessEqual(width, 422.0)
            self.assertLessEqual(height, 432.0)
            self.assertGreater(width, 0)
            self.assertGreater(height, 0)

    def test_degenerate_rect_still_gives_a_usable_card(self):
        width, height = overlay.card_size(0, 0, 422.0, 432.0)
        self.assertEqual((width, height), (422, 432))


class ThumbnailTests(unittest.TestCase):
    def test_downsizes_to_exactly_the_target(self):
        img = Image.new("RGB", (1920, 1080), (10, 20, 30))
        self.assertEqual(overlay.thumbnail_resize(img, (422, 238)).size, (422, 238))

    def test_handles_awkward_sizes(self):
        img = Image.new("RGB", (1921, 1081), (10, 20, 30))
        self.assertEqual(overlay.thumbnail_resize(img, (421, 237)).size, (421, 237))

    def test_upscales_a_window_smaller_than_its_card(self):
        img = Image.new("RGB", (80, 40), (10, 20, 30))
        self.assertEqual(overlay.thumbnail_resize(img, (400, 200)).size, (400, 200))

    def test_a_card_already_the_right_size_is_left_alone(self):
        img = Image.new("RGB", (400, 200), (10, 20, 30))
        self.assertIs(overlay.thumbnail_resize(img, (400, 200)), img)

    def test_a_heavy_reduction_keeps_the_picture(self):
        img = Image.new("RGB", (400, 400), (0, 0, 0))
        img.paste((255, 0, 0), (0, 0, 200, 200))
        img.paste((0, 255, 0), (200, 0, 400, 200))
        img.paste((0, 0, 255), (0, 200, 200, 400))
        img.paste((255, 255, 0), (200, 200, 400, 400))
        out = overlay.thumbnail_resize(img, (80, 80))
        self.assertEqual(out.size, (80, 80))
        self.assertGreater(out.getpixel((20, 20))[0], 200, "red quadrant lost")
        self.assertGreater(out.getpixel((60, 20))[1], 200, "green quadrant lost")
        self.assertGreater(out.getpixel((20, 60))[2], 200, "blue quadrant lost")
        self.assertGreater(out.getpixel((60, 60))[0], 200, "yellow quadrant lost")


class EaseTests(unittest.TestCase):
    def test_endpoints(self):
        self.assertAlmostEqual(overlay.ease_out(0.0), 0.0)
        self.assertAlmostEqual(overlay.ease_out(1.0), 1.0)

    def test_clamped_outside_the_unit_interval(self):
        self.assertAlmostEqual(overlay.ease_out(-3.0), 0.0)
        self.assertAlmostEqual(overlay.ease_out(4.0), 1.0)

    def test_eases_out_and_is_monotonic(self):
        self.assertGreater(overlay.ease_out(0.2), 0.45)
        values = [overlay.ease_out(i / 20.0) for i in range(21)]
        self.assertEqual(values, sorted(values))

    def test_lerp(self):
        self.assertAlmostEqual(overlay.lerp(10.0, 20.0, 0.25), 12.5)
        self.assertAlmostEqual(overlay.lerp(10.0, 20.0, 0.0), 10.0)
        self.assertAlmostEqual(overlay.lerp(10.0, 20.0, 1.0), 20.0)


class _StubCard:
    """Just enough card for the layout maths (no cairo surface needed)."""

    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height


def make_carousel(count: int = 4, size: tuple[int, int] = SCREEN, **overrides) -> overlay.Carousel:
    carousel = overlay.Carousel(Config(**overrides))
    carousel._width, carousel._height = size
    carousel._layout()
    card = overlay.card_size(1440, 900, carousel._slot_w, carousel._row_h)
    carousel._cards = [_StubCard(*card) for _ in range(count)]
    carousel._selection = 0
    carousel._sel_float = 0.0
    carousel._outline_w, carousel._outline_h = float(card[0]), float(card[1])
    carousel._view = carousel._view_from = carousel._view_to = 0.0
    return carousel


class StripLayoutTests(unittest.TestCase):
    def test_cards_are_one_pitch_apart(self):
        carousel = make_carousel(4)
        rects = carousel.card_rects()
        self.assertEqual(len(rects), 4)
        for first, second in zip(rects, rects[1:]):
            self.assertAlmostEqual(second[0] - first[0], carousel._pitch, places=6)

    def test_cards_are_vertically_centred_on_the_row(self):
        carousel = make_carousel(3)
        for x, y, w, h in carousel.card_rects():
            centre = y + h / 2.0
            self.assertAlmostEqual(centre, SCREEN[1] / 2.0, places=6)

    def test_row_is_symmetric_when_everything_fits(self):
        carousel = make_carousel(4)
        rects = carousel.card_rects()
        left_margin = rects[0][0]
        right_margin = SCREEN[0] - (rects[-1][0] + rects[-1][2])
        self.assertAlmostEqual(left_margin, right_margin, places=6)

    def test_selected_card_sits_in_the_middle_when_scrolling(self):
        carousel = make_carousel(8)
        carousel._selection = 5
        carousel._sel_float = 5.0
        carousel._view = carousel._view_to = overlay.view_target(5, 8, carousel._visible)
        x, _, w, _ = carousel.card_rects()[5]
        self.assertAlmostEqual(x + w / 2.0, SCREEN[0] / 2.0, places=6)

    def test_the_selected_card_is_always_fully_on_screen(self):
        """However many windows there are, the outlined one is never clipped."""
        carousel = make_carousel(9)
        for selection in range(9):
            carousel._selection = selection
            carousel._sel_float = float(selection)
            carousel._view = carousel._view_to = overlay.view_target(
                selection, 9, carousel._visible
            )
            x, _, w, _ = carousel.card_rects()[selection]
            self.assertGreaterEqual(x, -0.001, f"card {selection} ran off the left edge")
            self.assertLessEqual(x + w, SCREEN[0] + 0.001, f"card {selection} ran off the right")

    def test_the_row_is_not_scrolled_when_everything_fits(self):
        carousel = make_carousel(4)
        for selection in range(4):
            carousel._selection = selection
            carousel._sel_float = float(selection)
            carousel._view = carousel._view_to = overlay.view_target(
                selection, 4, carousel._visible
            )
            rects = carousel.card_rects()
            self.assertGreaterEqual(rects[0][0], 0.0)
            self.assertLessEqual(rects[-1][0] + rects[-1][2], SCREEN[0])

    def test_outline_hugs_the_selected_card(self):
        carousel = make_carousel(5)
        carousel._selection = 2
        carousel._sel_float = 2.0
        x, y, w, h = carousel.card_rects()[2]
        ox, oy, ow, oh = carousel.outline_rect()
        gap = carousel.config.highlight_gap_px
        self.assertAlmostEqual(ox, x - gap, places=6)
        self.assertAlmostEqual(oy, y - gap, places=6)
        self.assertAlmostEqual(ow, w + 2 * gap, places=6)
        self.assertAlmostEqual(oh, h + 2 * gap, places=6)

    def test_band_covers_the_whole_row(self):
        carousel = make_carousel(4)
        rects = carousel.card_rects()
        self.assertLessEqual(carousel._band_top, min(y for _, y, _, _ in rects))
        self.assertGreaterEqual(
            carousel._band_top + carousel._band_h,
            max(y + h for _, y, _, h in rects),
        )
        self.assertLess(carousel._band_top, 0.5 * SCREEN[1], "band should straddle the middle")

    def test_parallax_zero_leaves_the_row_still(self):
        carousel = make_carousel(4, row_parallax=0.0)
        carousel._progress = 1.7
        self.assertAlmostEqual(carousel._compute_tug(), 0.0)

    def test_parallax_tugs_by_a_fraction_of_a_slot(self):
        carousel = make_carousel(6, row_parallax=0.25)
        carousel._start_index = 2
        carousel._progress = 1.0
        self.assertAlmostEqual(carousel._compute_tug(), 0.25)
        carousel._progress = 40.0
        self.assertLessEqual(carousel._compute_tug(), 0.5, "the tug must stay a tug")
        self.assertGreaterEqual(carousel._compute_tug(), -0.5)


class CropTests(unittest.TestCase):
    def setUp(self):
        self.image = Image.new("RGB", (400, 300), (10, 20, 30))

    def test_crops_a_normal_rect(self):
        crop = overlay.Carousel._crop(self.image, (50, 60, 120, 80))
        self.assertEqual(crop.size, (120, 80))

    def test_clips_a_rect_that_hangs_off_the_screen(self):
        crop = overlay.Carousel._crop(self.image, (-100, -100, 300, 300))
        self.assertEqual(crop.size, (200, 200))

    def test_ignores_rects_entirely_off_the_screen(self):
        self.assertIsNone(overlay.Carousel._crop(self.image, (900, 900, 100, 100)))

    def test_ignores_tiny_rects(self):
        self.assertIsNone(overlay.Carousel._crop(self.image, (0, 0, 3, 3)))
        self.assertIsNone(overlay.Carousel._crop(self.image, (0, 0, 0, 0)))


class PrewarmCancelTests(unittest.TestCase):
    """Baking cards is ~30ms of PIL each, on the capture thread.

    When a swipe starts in the middle of that, the rest has to be abandoned:
    the frames the strip draws are worth more than a card that would arrive
    after the fingers are already moving. Nothing is swapped in that case, so
    the strip keeps the last complete set of cards.
    """

    def _carousel(self):
        carousel = overlay.Carousel(Config())
        carousel._width, carousel._height = 1920, 1080
        carousel._layout()
        return carousel

    def test_a_cancelled_prewarm_swaps_nothing_in(self):
        carousel = self._carousel()
        carousel.prewarm(
            Image.new("RGB", (400, 300), (10, 20, 30)),
            Image.new("RGB", (400, 300), (5, 5, 5)),
            1,
            {1: (0, 0, 400, 300), 2: (0, 0, 400, 300)},
            cancel=lambda: True,
        )
        self.assertEqual(carousel.generation, -1, "nothing was published")
        self.assertEqual(carousel._cards_by_xid, {})

    def test_a_prewarm_that_is_not_cancelled_still_lands(self):
        carousel = self._carousel()
        carousel.prewarm(
            Image.new("RGB", (400, 300), (10, 20, 30)),
            Image.new("RGB", (400, 300), (5, 5, 5)),
            7,
            {1: (0, 0, 400, 300)},
            cancel=lambda: False,
        )
        self.assertEqual(carousel.generation, 7)
        self.assertIn(1, carousel._cards_by_xid)


class _FakeGdkWindow:
    """Records what the overlay asks its X window to do."""

    def __init__(self, xid=0x5A00003):
        self._xid = xid
        self.calls: list[str] = []

    def get_xid(self):
        return self._xid

    def raise_(self):
        self.calls.append("raise")


class _FakeGtkWindow:
    def __init__(self, gdk_window):
        self._gdk_window = gdk_window
        self.realized = False
        self.mapped = False

    def realize(self):
        self.realized = True

    def show_all(self):
        self.mapped = True

    def get_window(self):
        return self._gdk_window


class OverlayRealizeTests(unittest.TestCase):
    """The strip has to be visible on *every* workspace, not just one.

    Muffin decides which workspace a window is on when it first maps it, so the
    "all desktops" property has to be written while the overlay is realized but
    still unmapped -- the id is handed to the app at exactly that moment. An
    overlay left on one workspace is invisible everywhere else: the gesture
    runs and the windows switch, but nothing shows which one you are choosing.
    And since arriving on a workspace restacks its windows, the strip is raised
    again every time it opens.
    """

    def test_the_id_is_handed_over_before_the_window_is_mapped(self):
        gdk_window = _FakeGdkWindow(0x5A00003)
        window = _FakeGtkWindow(gdk_window)
        seen: list[tuple[bool, int]] = []
        self.assertIs(
            overlay.realize_overlay(window, lambda xid: seen.append((window.realized, xid))),
            gdk_window,
        )
        self.assertEqual(seen, [(True, 0x5A00003)])
        self.assertFalse(window.mapped)

    def test_the_request_is_made_again_once_the_window_is_mapped(self):
        # The property is read at the first map; the _NET_WM_STATE message is
        # only honoured for a window the manager has already managed. Measured:
        # without this second request Muffin leaves the overlay on the
        # workspace it was born on and the strip is never seen anywhere else.
        gdk_window = _FakeGdkWindow(0x5A00003)
        window = _FakeGtkWindow(gdk_window)
        seen: list[tuple[bool, int]] = []
        overlay.map_overlay(window, gdk_window, lambda xid: seen.append((window.mapped, xid)))
        self.assertEqual(seen, [(True, 0x5A00003)])
        self.assertEqual(gdk_window.calls, ["raise"])

    def test_a_carousel_with_no_hook_still_maps(self):
        gdk_window = _FakeGdkWindow()
        window = _FakeGtkWindow(gdk_window)
        overlay.map_overlay(window, gdk_window, None)
        self.assertTrue(window.mapped)
        self.assertEqual(gdk_window.calls, ["raise"])

    def test_a_carousel_with_no_hook_still_realizes(self):
        gdk_window = _FakeGdkWindow()
        window = _FakeGtkWindow(gdk_window)
        self.assertIs(overlay.realize_overlay(window, None), gdk_window)
        self.assertTrue(window.realized)

    def test_no_gdk_window_is_an_error(self):
        with self.assertRaises(RuntimeError):
            overlay.realize_overlay(_FakeGtkWindow(None))

    def test_the_sticky_request_is_retried_after_the_map(self):
        seen: list[int] = []
        carousel = overlay.Carousel(Config(), make_sticky=seen.append)
        carousel.window = _FakeGtkWindow(_FakeGdkWindow(0x5A00003))
        for _ in range(overlay.STICKY_RETRIES + 2):
            carousel._retry_sticky()
        self.assertEqual(seen, [0x5A00003] * overlay.STICKY_RETRIES)

    def test_opening_the_strip_raises_it_again_and_reasserts_sticky(self):
        gdk_window = _FakeGdkWindow(0x5A00003)
        seen: list[int] = []
        carousel = overlay.Carousel(Config(), make_sticky=seen.append)
        carousel.window = _FakeGtkWindow(gdk_window)
        carousel._keep_on_top()
        self.assertEqual(gdk_window.calls, ["raise"])
        self.assertEqual(seen, [0x5A00003])

    def test_keeping_on_top_without_a_window_is_harmless(self):
        carousel = overlay.Carousel(Config())
        carousel.window = _FakeGtkWindow(None)
        carousel._keep_on_top()  # must not raise: the window may be gone


if __name__ == "__main__":
    unittest.main()
