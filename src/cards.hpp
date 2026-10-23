// Cards: one window's frozen picture, baked bright and dimmed.
//
// Runs on the capture worker thread and touches no GTK object -- only Cairo
// image surfaces -- so a card can be prepared while the strip is not on screen.
#pragma once

#include <cairo.h>

#include <optional>
#include <string>
#include <utility>

#include "config.hpp"
#include "image.hpp"
#include "x11.hpp"

namespace wintab {

struct Card {
    std::string key;
    std::string label;
    WindowId xid = 0;
    int width = 0;
    int height = 0;
    int padding = 0;
    Image surface;
    Image dim_surface;
    bool has_pixels = true;
    //: True when cropped out of a screen grab, so it is only valid for the
    //: workspace that grab was taken on.
    bool from_screen = false;
};

//: Size of a card inside its slot: aspect preserved, bounded by the slot.
std::pair<int, int> card_size(double window_w, double window_h, double slot_w, double row_h);

//: Where the name under a card sits, as a Cairo baseline.
double caption_baseline(double card_top, double card_height, double gap_px, double font_px);

//: A rounded rectangle path, exactly the arcs the Python renderer draws.
void rounded_rect(cairo_t* context, double x, double y, double w, double h, double radius);

class CardBaker {
public:
    CardBaker(const Config& config, double slot_w, double row_h);

    std::optional<Card> make_card(WindowId xid, const std::string& key, const Rect& rect,
                                  const Image* source, const std::string& label = "",
                                  bool from_screen = false) const;

    const Config& config() const { return config_; }

private:
    Image compose(const Image* body, int card_w, int card_h, int padding,
                  double placeholder_bias) const;
    void paint_shadow(cairo_t* context, double x, double y, double w, double h) const;

    const Config& config_;
    double slot_w_ = 0.0;
    double row_h_ = 0.0;
};

}  // namespace wintab
