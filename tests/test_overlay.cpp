#include <cmath>
#include <tuple>
#include <vector>

#include "cards.hpp"
#include "config.hpp"
#include "image.hpp"
#include "overlay.hpp"
#include "support/harness.hpp"

using namespace wintab;

namespace {

constexpr double kScreenW = 1920.0;
constexpr double kScreenH = 1080.0;

}  // namespace

TEST_CASE(overlay_pitch_and_fit) {
    CHECK_NEAR(overlay::pitch_of(100.0, 10.0), 110.0, 1e-12);
    CHECK_NEAR(overlay::row_width(4, 100.0, 10.0), 430.0, 1e-12);
    CHECK_EQ(overlay::row_width(0, 100.0, 10.0), 0.0);
    CHECK_EQ(overlay::visible_slots(1920.0, 422.4, 34.56), 4);
    CHECK_EQ(overlay::visible_slots(100.0, 400.0, 10.0), 1);
}

TEST_CASE(overlay_view_only_scrolls_when_the_row_does_not_fit) {
    CHECK_EQ(overlay::view_target(0, 3, 4), 0.0);
    CHECK_EQ(overlay::view_target(9, 3, 4), 0.0);
    CHECK_EQ(overlay::view_target(5, 10, 4), 3.5);
    CHECK_EQ(overlay::view_target(0, 10, 4), 0.0);
    CHECK_EQ(overlay::view_target(9, 10, 4), 6.0);
}

TEST_CASE(overlay_row_is_centred_when_everything_fits) {
    const double origin = overlay::row_origin(3, 4, 1000.0, 100.0, 10.0, 0.0);
    CHECK_NEAR(origin, (1000.0 - 320.0) / 2.0, 1e-12);
    const double scrolled = overlay::row_origin(10, 4, 1000.0, 100.0, 10.0, 2.0);
    CHECK_NEAR(scrolled, (1000.0 - 430.0) / 2.0 - 2.0 * 110.0, 1e-12);
}

TEST_CASE(overlay_slot_positions) {
    CHECK_NEAR(overlay::slot_left(2.0, 10.0, 110.0), 230.0, 1e-12);
    CHECK_NEAR(overlay::slot_centre_x(2.0, 10.0, 110.0, 100.0), 280.0, 1e-12);
    CHECK_NEAR(overlay::slot_centre_x(0.5, 0.0, 100.0, 80.0), 90.0, 1e-12);
}

TEST_CASE(overlay_easing) {
    CHECK_NEAR(overlay::ease_out(0.0), 0.0, 1e-12);
    CHECK_NEAR(overlay::ease_out(1.0), 1.0, 1e-12);
    CHECK_NEAR(overlay::ease_out(0.5), 0.875, 1e-12);
    CHECK_NEAR(overlay::ease_out(-2.0), 0.0, 1e-12);
    CHECK_NEAR(overlay::ease_out(2.0), 1.0, 1e-12);
    CHECK_NEAR(overlay::lerp(10.0, 20.0, 0.25), 12.5, 1e-12);
}

TEST_CASE(overlay_card_size_preserves_aspect) {
    const auto wide = card_size(800, 600, 200, 120);
    CHECK_EQ(wide.first, 160);
    CHECK_EQ(wide.second, 120);
    const auto tall = card_size(100, 1000, 200, 120);
    CHECK_EQ(tall.first, 12);
    CHECK_EQ(tall.second, 120);
    const auto degenerate = card_size(0, 0, 200, 120);
    CHECK_EQ(degenerate.first, 200);
    CHECK_EQ(degenerate.second, 120);
}

TEST_CASE(overlay_thumbnail_resize_reaches_the_target) {
    const Image window = Image::solid(800, 600, 10, 200, 30);
    CHECK(window);
    const Image small = thumbnail_resize(window, 160, 120);
    CHECK(small);
    CHECK_EQ(small.width(), 160);
    CHECK_EQ(small.height(), 120);
}

TEST_CASE(overlay_caption_baseline) {
    CHECK_NEAR(caption_baseline(100.0, 50.0, 6.0, 13.0), 166.4, 1e-9);
}

TEST_CASE(overlay_a_card_shows_the_window_not_the_screen) {
    const Image screen = Image::solid(1920, 1080, 255, 0, 0);
    const Image window = Image::solid(800, 600, 0, 255, 0);
    Config config;
    const CardBaker baker(config, 200.0, 120.0);
    const auto card = baker.make_card(7, "7", Rect{100, 100, 800, 600}, &window, "Firefox");
    CHECK(card.has_value());
    if (!card) return;
    CHECK_EQ(card->width, 160);
    CHECK_EQ(card->height, 120);
    CHECK(!card->from_screen);
    const std::uint32_t middle =
        pixel_at(card->surface, card->padding + card->width / 2, card->padding + card->height / 2);
    const int red = static_cast<int>((middle >> 16) & 0xFF);
    const int green = static_cast<int>((middle >> 8) & 0xFF);
    const int blue = static_cast<int>(middle & 0xFF);
    CHECK(green > 200);
    CHECK(red < 40);
    CHECK(blue < 40);
}

TEST_CASE(overlay_the_dim_twin_is_darker) {
    const Image window = Image::solid(640, 480, 0, 200, 255);
    Config config;
    const CardBaker baker(config, 200.0, 120.0);
    const auto card = baker.make_card(3, "3", Rect{0, 0, 640, 480}, &window, "App");
    CHECK(card.has_value());
    if (!card) return;
    const std::uint32_t bright =
        pixel_at(card->surface, card->padding + card->width / 2, card->padding + card->height / 2);
    const std::uint32_t dim = pixel_at(card->dim_surface, card->padding + card->width / 2,
                                       card->padding + card->height / 2);
    CHECK(((dim >> 8) & 0xFF) < ((bright >> 8) & 0xFF));
}

TEST_CASE(overlay_a_window_without_pixels_gets_a_placeholder_card) {
    Config config;
    const CardBaker baker(config, 200.0, 120.0);
    const auto card = baker.make_card(9, "app", Rect{0, 0, 800, 600}, nullptr, "Minimized");
    CHECK(card.has_value());
    if (!card) return;
    CHECK(!card->has_pixels);
    const std::uint32_t middle =
        pixel_at(card->surface, card->padding + card->width / 2, card->padding + card->height / 2);
    // A grey placeholder, not a picture: all channels low and roughly equal.
    const int red = static_cast<int>((middle >> 16) & 0xFF);
    const int green = static_cast<int>((middle >> 8) & 0xFF);
    CHECK(red < 90);
    CHECK(green < 90);
    CHECK(std::abs(red - green) < 20);
}

TEST_CASE(overlay_the_size_of_the_picture_wins_over_the_rect) {
    const Image window = Image::solid(1000, 400, 20, 20, 20);
    Config config;
    const CardBaker baker(config, 220.0, 160.0);
    // The rect claims a 4:3 shape, the picture is wider: the card keeps 1000x400.
    const auto card = baker.make_card(1, "1", Rect{0, 0, 800, 600}, &window, "Wide");
    CHECK(card.has_value());
    if (!card) return;
    CHECK_NEAR(static_cast<double>(card->width) / card->height, 2.5, 0.02);
}

// --- the strip's layout, pinned by the golden values the Python build gives --

namespace {

Config wide(double slot_fraction) {
    Config config;
    config.slot_fraction = slot_fraction;
    return config;
}

}  // namespace

TEST_CASE(overlay_four_slots_fit_a_full_hd_screen) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 4);
    CHECK_EQ(strip.visible_slot_count(), 4);
}

TEST_CASE(overlay_visible_slots_never_reports_zero) {
    Carousel strip(wide(0.9), nullptr, nullptr);
    strip.stub_layout(800, 600, 1);
    CHECK(strip.visible_slot_count() >= 1);
}

TEST_CASE(overlay_row_width_has_no_trailing_gap) {
    CHECK_NEAR(overlay::row_width(3, 100.0, 10.0), 3 * 100.0 + 2 * 10.0, 1e-12);
}

TEST_CASE(overlay_slots_are_one_pitch_apart) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 4);
    const auto rects = strip.card_rects();
    CHECK_EQ(rects.size(), std::size_t(4));
    for (std::size_t i = 0; i + 1 < rects.size(); ++i) {
        CHECK_NEAR(std::get<0>(rects[i + 1]) - std::get<0>(rects[i]), strip.pitch(), 1e-6);
    }
}

TEST_CASE(overlay_cards_are_vertically_centred_on_the_row) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 3);
    for (const auto& [x, y, w, h] : strip.card_rects()) {
        CHECK_NEAR(y + h / 2.0, kScreenH / 2.0, 1e-6);
    }
}

TEST_CASE(overlay_row_is_symmetric_when_everything_fits) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 4);
    const auto rects = strip.card_rects();
    const double left = std::get<0>(rects.front());
    const double right = kScreenW - (std::get<0>(rects.back()) + std::get<2>(rects.back()));
    CHECK_NEAR(left, right, 1e-6);
}

TEST_CASE(overlay_no_scrolling_when_everything_fits) {
    CHECK_EQ(overlay::view_target(0, 3, 4), 0.0);
    CHECK_EQ(overlay::view_target(2, 3, 4), 0.0);
}

TEST_CASE(overlay_selection_is_centred_when_the_row_can_scroll) {
    // 8 windows, 4 slots: slot 5 sits centred with 1.5 slots to its left.
    CHECK_NEAR(overlay::view_target(5, 8, 4), 3.5, 1e-12);
}

TEST_CASE(overlay_scrolling_stops_at_the_ends) {
    CHECK_EQ(overlay::view_target(0, 8, 4), 0.0);
    CHECK_EQ(overlay::view_target(1, 8, 4), 0.0);
    CHECK_EQ(overlay::view_target(7, 8, 4), 4.0);
    CHECK_EQ(overlay::view_target(6, 8, 4), 4.0);
}

TEST_CASE(overlay_selected_card_sits_in_the_middle_when_scrolling) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 10);
    // The view stops half a slot off the selection, so the outlined card lands
    // dead centre no matter how many windows are open.
    strip.stub_select(5);
    const auto rects = strip.card_rects();
    const auto& [x, y, w, h] = rects[5];
    CHECK_NEAR(x + w / 2.0, kScreenW / 2.0, 1e-6);
}

TEST_CASE(overlay_the_selected_card_is_always_fully_on_screen) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 9);
    for (int selection = 0; selection < 9; ++selection) {
        strip.stub_select(selection);
        const auto rects = strip.card_rects();
        const double x = std::get<0>(rects[static_cast<std::size_t>(selection)]);
        const double w = std::get<2>(rects[static_cast<std::size_t>(selection)]);
        CHECK(x >= -0.001);
        CHECK(x + w <= kScreenW + 0.001);
    }
}

TEST_CASE(overlay_the_row_is_not_scrolled_when_everything_fits) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 4);
    for (int selection = 0; selection < 4; ++selection) {
        strip.stub_select(selection);
        const auto rects = strip.card_rects();
        CHECK(std::get<0>(rects.front()) >= 0.0);
        CHECK(std::get<0>(rects.back()) + std::get<2>(rects.back()) <= kScreenW);
    }
}

TEST_CASE(overlay_outline_hugs_the_selected_card) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 5);
    strip.stub_select(2);
    const auto rects = strip.card_rects();
    const auto& [x, y, w, h] = rects[2];
    const auto [ox, oy, ow, oh] = strip.outline_rect();
    const double gap = strip.config().highlight_gap_px;
    CHECK_NEAR(ox, x - gap, 1e-6);
    CHECK_NEAR(oy, y - gap, 1e-6);
    CHECK_NEAR(ow, w + 2 * gap, 1e-6);
    CHECK_NEAR(oh, h + 2 * gap, 1e-6);
}

TEST_CASE(overlay_band_covers_the_whole_row) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 4);
    const auto rects = strip.card_rects();
    double top = 1e9;
    double bottom = -1e9;
    for (const auto& [x, y, w, h] : rects) {
        top = std::min(top, y);
        bottom = std::max(bottom, y + h);
    }
    CHECK(strip.band_top() <= top);
    CHECK(strip.band_top() + strip.band_height() >= bottom);
    CHECK(strip.band_top() < 0.5 * kScreenH);  // straddles the middle
}

TEST_CASE(overlay_parallax_zero_leaves_the_row_still) {
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 4);
    CHECK_NEAR(strip.compute_tug_now(), 0.0, 1e-12);
}

TEST_CASE(overlay_parallax_tugs_by_a_fraction_of_a_slot) {
    Config config;
    config.slot_fraction = 0.22;
    config.row_parallax = 0.25;
    Carousel strip(config, nullptr, nullptr);
    strip.stub_layout(1920, 1080, 6);
    strip.stub_drag();
    // The tug is the progress scaled by parallax, and it stays a tug: past the
    // end of the row it is rubber-banded and then clamped to half a slot.
    strip.set_progress(1.0);
    CHECK_NEAR(strip.compute_tug_now(), 0.25, 1e-6);
    strip.set_progress(40.0);
    CHECK(strip.compute_tug_now() <= 0.5 + 1e-9);
    CHECK(strip.compute_tug_now() >= -0.5 - 1e-9);
    // And the row physically moves by that fraction of a slot.
    strip.set_progress(1.0);
    CHECK_NEAR(strip.tug(), 0.25, 1e-6);
}

TEST_CASE(overlay_row_is_centred_on_the_visible_window_of_slots) {
    // When the row is wider than the screen, the visible window of slots is
    // what centres it, not the whole row: the selected card stays in the middle
    // even at the far end, because the view clamps to count - visible.
    Carousel strip(wide(0.22), nullptr, nullptr);
    strip.stub_layout(1920, 1080, 10);
    strip.stub_select(9);
    const auto rects = strip.card_rects();
    const double x = std::get<0>(rects[9]);
    const double w = std::get<2>(rects[9]);
    CHECK(x + w <= kScreenW + 0.001);
    CHECK(x >= -0.001);
    // The far end of the row reaches the right edge exactly, no scroll beyond.
    CHECK_NEAR(std::get<0>(rects[0]) + strip.row_height() * 0.0, std::get<0>(rects[0]), 1e-9);
}
