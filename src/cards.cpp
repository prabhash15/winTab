#include "cards.hpp"

#include <algorithm>
#include <cmath>

namespace wintab {

std::pair<int, int> card_size(double window_w, double window_h, double slot_w, double row_h) {
    if (window_w <= 0.0 || window_h <= 0.0 || slot_w <= 0.0 || row_h <= 0.0) {
        return {std::max(1, static_cast<int>(slot_w)), std::max(1, static_cast<int>(row_h))};
    }
    const double aspect = window_w / window_h;
    const double height = std::min(row_h, slot_w / aspect);
    const double width = height * aspect;
    // Python's round() is half-to-even; nearbyint with the default mode is too.
    return {std::max(1, static_cast<int>(std::nearbyint(width))),
            std::max(1, static_cast<int>(std::nearbyint(height)))};
}

double caption_baseline(double card_top, double card_height, double gap_px, double font_px) {
    // 0.8 of the font size is the usual ascender-to-baseline ratio.
    return card_top + card_height + gap_px + font_px * 0.8;
}

void rounded_rect(cairo_t* context, double x, double y, double w, double h, double radius) {
    if (w <= 0.0 || h <= 0.0) return;
    radius = std::max(0.0, std::min({radius, w / 2.0, h / 2.0}));
    if (radius <= 0.5) {
        cairo_rectangle(context, x, y, w, h);
        return;
    }
    const double pi = 3.14159265358979323846;
    cairo_new_sub_path(context);
    cairo_arc(context, x + w - radius, y + radius, radius, -pi / 2.0, 0.0);
    cairo_arc(context, x + w - radius, y + h - radius, radius, 0.0, pi / 2.0);
    cairo_arc(context, x + radius, y + h - radius, radius, pi / 2.0, pi);
    cairo_arc(context, x + radius, y + radius, radius, pi, 1.5 * pi);
    cairo_close_path(context);
}

CardBaker::CardBaker(const Config& config, double slot_w, double row_h)
    : config_(config), slot_w_(slot_w), row_h_(row_h) {}

void CardBaker::paint_shadow(cairo_t* context, double x, double y, double w, double h) const {
    const double spread = config_.card_shadow_px;
    if (spread <= 0.0) return;
    const int layers = std::max(3, static_cast<int>(std::floor(spread / 9.0)));
    cairo_save(context);
    for (int layer = layers; layer > 0; --layer) {
        const double grow = spread * (static_cast<double>(layer) / layers);
        cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.05);
        rounded_rect(context, x - grow, y - grow + grow * 0.30, w + grow * 2.0, h + grow * 2.0,
                     config_.card_radius + grow);
        cairo_fill(context);
    }
    cairo_restore(context);
}

Image CardBaker::compose(const Image* body, int card_w, int card_h, int padding,
                         double placeholder_bias) const {
    Image surface = Image::create(card_w + 2 * padding, card_h + 2 * padding);
    if (!surface) return Image();
    cairo_t* context = cairo_create(surface.surface());
    if (!context) return Image();

    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);

    paint_shadow(context, padding, padding, card_w, card_h);

    cairo_save(context);
    rounded_rect(context, padding, padding, card_w, card_h, config_.card_radius);
    cairo_clip(context);
    if (body) {
        cairo_set_source_surface(context, body->surface(), padding, padding);
        cairo_paint(context);
    } else {
        const double shade = std::min(0.5, 0.12 + placeholder_bias);
        cairo_set_source_rgba(context, shade + 0.06, shade + 0.07, shade + 0.10, 1.0);
        cairo_rectangle(context, padding, padding, card_w, card_h);
        cairo_fill(context);
    }
    cairo_restore(context);

    if (!body) {
        // A window with no pixels (minimized, or captured before it existed)
        // still gets a card, so the strip never jumps.
        cairo_save(context);
        rounded_rect(context, padding + 1, padding + 1, card_w - 2, card_h - 2,
                     config_.card_radius);
        cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 0.10 - placeholder_bias * 0.2);
        cairo_set_line_width(context, 2.0);
        cairo_stroke(context);
        cairo_restore(context);
    }

    cairo_destroy(context);
    cairo_surface_flush(surface.surface());
    return surface;
}

std::optional<Card> CardBaker::make_card(WindowId xid, const std::string& key, const Rect& rect,
                                         const Image* source, const std::string& label,
                                         bool from_screen) const {
    int window_w = rect.w;
    int window_h = rect.h;
    if (source && source->width() > 0 && source->height() > 0) {
        // The picture's own size wins: a window's off-screen pixels include its
        // decoration, so they are the true shape of the thing.
        window_w = source->width();
        window_h = source->height();
    }

    const auto [card_w, card_h] = card_size(window_w, window_h, slot_w_, row_h_);
    const int padding = static_cast<int>(config_.card_shadow_px) + 2;

    Image body;
    if (source && *source) body = thumbnail_resize(*source, card_w, card_h);

    Image dim_body;
    if (body && config_.dim_others > 0.0) {
        dim_body = blend_to_black(body, std::min(1.0, std::max(0.0, config_.dim_others)));
    }

    Image bright = compose(body ? &body : nullptr, card_w, card_h, padding, 0.0);
    if (!bright) return std::nullopt;
    Image dim = bright;
    if (dim_body) {
        dim = compose(&dim_body, card_w, card_h, padding, 0.35);
        if (!dim) dim = bright;
    }

    Card card;
    card.key = key;
    card.label = label;
    card.xid = xid;
    card.width = card_w;
    card.height = card_h;
    card.padding = padding;
    card.surface = bright;
    card.dim_surface = dim;
    card.has_pixels = static_cast<bool>(body);
    card.from_screen = from_screen;
    return card;
}

}  // namespace wintab
