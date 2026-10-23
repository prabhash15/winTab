#include "overlay.hpp"

#include <gdk/gdkx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "gesture.hpp"
#include "log.hpp"

namespace wintab::overlay {
namespace {

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

}  // namespace

double pitch_of(double slot_w, double gap) { return slot_w + gap; }

int visible_slots(double screen_w, double slot_w, double gap) {
    if (screen_w <= 0.0 || slot_w <= 0.0) return 1;
    const double pitch = pitch_of(slot_w, gap);
    return std::max(1, static_cast<int>(std::floor((screen_w + gap) / pitch)));
}

double row_width(int count, double slot_w, double gap) {
    if (count <= 0) return 0.0;
    return count * slot_w + (count - 1) * gap;
}

double view_target(int selection, int count, int visible) {
    if (count <= visible) return 0.0;
    // Deliberately fractional: the row stops half a slot off the selection so
    // the outlined card lands dead centre (a whole slot here would leave the
    // strip up to half a slot -- ~228px on a 1920 screen -- off centre).
    return std::max(0.0, std::min(static_cast<double>(count - visible),
                                  selection - (visible - 1) / 2.0));
}

double row_origin(int count, int visible, double screen_w, double slot_w, double gap, double view) {
    if (count <= 0) return 0.0;
    if (count <= visible) return (screen_w - row_width(count, slot_w, gap)) / 2.0;
    const double visible_width = row_width(visible, slot_w, gap);
    return (screen_w - visible_width) / 2.0 - view * pitch_of(slot_w, gap);
}

double slot_left(double index, double origin, double pitch) { return origin + index * pitch; }

double slot_centre_x(double index, double origin, double pitch, double slot_w) {
    return slot_left(index, origin, pitch) + slot_w / 2.0;
}

double ease_out(double t) {
    t = std::min(1.0, std::max(0.0, t));
    return 1.0 - std::pow(1.0 - t, 3.0);
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace wintab::overlay

namespace wintab {
namespace {

using overlay::now_seconds;

constexpr int kStickyRetryMs = 400;
constexpr int kStickyRetries = 3;
constexpr int kAttentionClearDelayMs = 1200;
constexpr int kAttentionClearRetries = 4;

}  // namespace

Carousel::Carousel(const Config& config, StickyFn make_sticky, AttentionFn clear_attention)
    : config_(config),
      make_sticky_(std::move(make_sticky)),
      clear_attention_callback_(std::move(clear_attention)) {}

Carousel::~Carousel() { close(); }

int Carousel::generation() const {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    return buffer_generation_;
}

// --- window ---------------------------------------------------------------

GtkWidget* Carousel::ensure_window() {
    if (window_) return window_;

    GdkScreen* screen = gdk_screen_get_default();
    if (!screen) throw std::runtime_error("no Gdk screen: is DISPLAY set?");
    // The overlay spans the whole X screen, monitors included, so ask Xlib:
    // gdk_screen_get_width() is the same number but deprecated since 3.22.
    Display* xdisplay = GDK_DISPLAY_XDISPLAY(gdk_screen_get_display(screen));
    const int screen_number = gdk_x11_screen_get_screen_number(screen);
    screen_width_ = DisplayWidth(xdisplay, screen_number);
    screen_height_ = DisplayHeight(xdisplay, screen_number);
    layout();

    GtkWidget* win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    // NOTE: keep-above is deliberately NOT set here; the strip's visibility is
    // a gesture's lifetime, so open() pins ABOVE and the close paths hand the
    // layer back. A permanently above overlay makes every newly opened
    // application "land behind" (focus-stealing prevention).
    GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(win, visual);
    gtk_widget_set_app_paintable(win, TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), screen_width_, screen_height_);
    gtk_window_move(GTK_WINDOW(win), 0, 0);
    gtk_widget_set_opacity(win, 0.0);
    g_signal_connect(win, "draw", G_CALLBACK(&Carousel::on_draw_cb), this);
    g_signal_connect(win, "destroy", G_CALLBACK(&Carousel::on_destroy_cb), this);

    // Realize BEFORE mapping: the window manager reads _NET_WM_DESKTOP once,
    // at first map, and a window it places on one workspace is not composited
    // anywhere else.
    gtk_widget_realize(win);
    GdkWindow* gdk_window = gtk_widget_get_window(win);
    if (!gdk_window) throw std::runtime_error("overlay window was not created");
    const WindowId xid = static_cast<WindowId>(gdk_x11_window_get_xid(gdk_window));
    if (make_sticky_) make_sticky_(xid);

    // Empty input shape: nothing the overlay draws can be clicked.
    cairo_region_t* empty = cairo_region_create();
    gdk_window_input_shape_combine_region(gdk_window, empty, 0, 0);
    cairo_region_destroy(empty);

    gtk_widget_show_all(win);
    gdk_window_raise(gdk_window);
    if (make_sticky_) make_sticky_(xid);
    retry_sticky_soon();
    clear_attention_soon();

    window_ = win;
    return win;
}

void Carousel::close() {
    if (!window_) return;
    stop_ticks();
    GtkWidget* window = window_;
    window_ = nullptr;
    gtk_widget_destroy(window);
}

void Carousel::layout() {
    slot_w_ = screen_width_ * config_.slot_fraction;
    gap_ = screen_width_ * config_.card_gap_fraction;
    pitch_ = overlay::pitch_of(slot_w_, gap_);
    visible_ = overlay::visible_slots(screen_width_, slot_w_, gap_);
    row_h_ = screen_height_ * config_.max_card_height_fraction;
    row_top_ = (screen_height_ - row_h_) / 2.0;
    band_top_ = row_top_ - config_.band_pad_px;
    band_h_ = row_h_ + 2.0 * config_.band_pad_px;
}

void Carousel::assert_sticky() {
    if (!make_sticky_ || !window_) return;
    GdkWindow* gdk_window = gtk_widget_get_window(window_);
    if (gdk_window) make_sticky_(static_cast<WindowId>(gdk_x11_window_get_xid(gdk_window)));
}

void Carousel::retry_sticky_soon() {
    if (!make_sticky_ || sticky_tries_ >= kStickyRetries) return;
    g_timeout_add(kStickyRetryMs, &Carousel::sticky_retry_cb, this);
}

gboolean Carousel::sticky_retry_cb(gpointer data) {
    auto* self = static_cast<Carousel*>(data);
    if (self->sticky_tries_ >= kStickyRetries) return G_SOURCE_REMOVE;
    self->sticky_tries_ += 1;
    self->assert_sticky();
    // A sticky re-assertion re-runs the state machinery that stamps the
    // attention flag on, so the two requests ride together.
    self->clear_attention_soon();
    return self->sticky_tries_ < kStickyRetries ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void Carousel::clear_attention_soon() {
    g_timeout_add(kAttentionClearDelayMs, &Carousel::attention_retry_cb, this);
}

gboolean Carousel::attention_retry_cb(gpointer data) {
    auto* self = static_cast<Carousel*>(data);
    if (self->attention_clears_ >= kAttentionClearRetries) return G_SOURCE_REMOVE;
    if (!self->window_) return G_SOURCE_REMOVE;
    GdkWindow* gdk_window = gtk_widget_get_window(self->window_);
    if (gdk_window) {
        self->clear_attention_now(static_cast<WindowId>(gdk_x11_window_get_xid(gdk_window)));
    }
    self->attention_clears_ += 1;
    return self->attention_clears_ < kAttentionClearRetries ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

void Carousel::clear_attention_now(WindowId xid) {
    if (clear_attention_callback_) clear_attention_callback_(xid);
}

void Carousel::keep_on_top() {
    assert_sticky();
    if (!window_) return;
    GdkWindow* gdk_window = gtk_widget_get_window(window_);
    if (!gdk_window) return;
    gdk_window_set_keep_above(gdk_window, TRUE);
    gdk_window_raise(gdk_window);
}

void Carousel::release_on_top() {
    if (!window_) return;
    GdkWindow* gdk_window = gtk_widget_get_window(window_);
    if (gdk_window) gdk_window_set_keep_above(gdk_window, FALSE);
}

// --- pixels (capture thread) ---------------------------------------------

Image Carousel::build_backdrop(const Image& plain) {
    // The screen grab is already RGB24 and immutable, so this is a refcount
    // bump rather than Pillow's convert+copy.
    return plain;
}

void Carousel::prewarm(const Image& plain, const Image& backdrop, int generation,
                       const std::map<WindowId, Rect>& window_rects,
                       const std::map<WindowId, Image>& window_pixels, bool allow_crop,
                       const std::function<bool()>& cancel) {
    if (screen_width_ <= 0 || screen_height_ <= 0) return;  // window not created yet

    Image prepared_backdrop = build_backdrop(backdrop ? backdrop : plain);
    CardBaker baker(config_, slot_w_, row_h_);
    std::map<WindowId, Card> cards;

    for (const auto& [xid, rect] : window_rects) {
        if (cancel && cancel()) return;
        const auto own = window_pixels.find(xid);
        Image cropped;
        const Image* source = nullptr;
        const bool from_screen = own == window_pixels.end();
        if (!from_screen) {
            source = &own->second;
        } else {
            if (!allow_crop || !plain) continue;
            cropped = plain.crop(rect);
            if (!cropped) continue;
            source = &cropped;
        }
        if (auto card = baker.make_card(xid, std::to_string(xid), rect, source, "", from_screen)) {
            cards[xid] = *card;
        }
    }

    for (const auto& [xid, picture] : window_pixels) {
        if (cancel && cancel()) return;
        if (cards.count(xid)) continue;
        // A window we have pixels for but no rect for still deserves a card.
        const Rect rect{0, 0, picture.width(), picture.height()};
        if (auto card = baker.make_card(xid, std::to_string(xid), rect, &picture)) {
            cards[xid] = *card;
        }
    }

    std::lock_guard<std::mutex> lock(buffers_mutex_);
    backdrop_ = prepared_backdrop;
    backdrop_generation_ = generation;
    cards_by_xid_ = std::move(cards);
    placeholders_.clear();
    buffer_generation_ = generation;
}

// --- gesture API ----------------------------------------------------------

std::optional<Card> Carousel::card_for(const AppEntry& entry) {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    for (const WindowRef& ref : entry.windows) {
        const auto found = cards_by_xid_.find(ref.xid);
        if (found != cards_by_xid_.end()) {
            // The entry is the one source of names.
            found->second.label = entry.label;
            return found->second;
        }
    }
    const auto existing = placeholders_.find(entry.key);
    if (existing != placeholders_.end()) return existing->second;

    CardBaker baker(config_, slot_w_, row_h_);
    auto placeholder =
        baker.make_card(entry.top().xid, entry.key, entry.top().rect, nullptr, entry.label);
    if (!placeholder) return std::nullopt;
    placeholders_.emplace(entry.key, *placeholder);
    return placeholder;
}

void Carousel::open(const std::vector<AppEntry>& entries, int index) {
    ensure_window();
    layout();

    std::vector<Card> cards;
    for (const AppEntry& entry : entries) {
        if (auto card = card_for(entry)) cards.push_back(*card);
    }
    cards_ = std::move(cards);
    if (cards_.empty()) {
        state_ = "idle";
        return;
    }

    start_index_ = std::max(0, std::min(static_cast<int>(cards_.size()) - 1, index));
    selection_ = start_index_;
    progress_ = 0.0;
    tug_ = 0.0;
    sel_float_ = static_cast<double>(selection_);
    const Card& card = cards_[static_cast<std::size_t>(selection_)];
    outline_w_ = card.width;
    outline_h_ = card.height;
    outline_from_ = outline_to_ = snapshot_outline();
    view_ = view_from_ = view_to_ =
        overlay::view_target(selection_, static_cast<int>(cards_.size()), visible_);
    sliding_ = false;
    frame_max_ms_ = 0.0;
    state_ = "drag";

    GtkWidget* win = window_or_create();
    gtk_widget_set_opacity(win, 1.0);
    keep_on_top();
    // A manager that (re)appeared while winTab was running stamps the attention
    // flag on when it takes the window on, so withdraw it immediately too.
    clear_attention_soon();
    gtk_widget_queue_draw(win);
}

GtkWidget* Carousel::window_or_create() { return window_ ? window_ : ensure_window(); }

void Carousel::forget_screen_cards() {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    for (auto it = cards_by_xid_.begin(); it != cards_by_xid_.end();) {
        if (it->second.from_screen) {
            it = cards_by_xid_.erase(it);
        } else {
            ++it;
        }
    }
    placeholders_.clear();
}

void Carousel::set_progress(double progress, std::optional<int> steps) {
    if (state_ != "drag" || cards_.empty()) return;
    progress_ = progress;
    int target_steps = 0;
    if (steps) {
        target_steps = *steps;
    } else {
        target_steps = highlight_index(progress, start_index_, static_cast<int>(cards_.size())) -
                       start_index_;
    }
    move_selection(start_index_ + target_steps);

    const double tug = compute_tug();
    if (std::abs(tug - tug_) * pitch_ >= 0.25) {
        tug_ = tug;
        if (window_) gtk_widget_queue_draw(window_);
    }
}

void Carousel::move_selection(int selection) {
    if (cards_.empty()) return;
    selection = std::max(0, std::min(static_cast<int>(cards_.size()) - 1, selection));
    if (selection == selection_) return;

    const Card& previous = cards_[static_cast<std::size_t>(selection_)];
    const Card& target = cards_[static_cast<std::size_t>(selection)];
    selection_ = selection;
    outline_from_ = {sel_float_, static_cast<double>(previous.width),
                     static_cast<double>(previous.height)};
    outline_to_ = {static_cast<double>(selection), static_cast<double>(target.width),
                   static_cast<double>(target.height)};
    view_from_ = view_;
    view_to_ = overlay::view_target(selection, static_cast<int>(cards_.size()), visible_);
    slide_t0_ = now_seconds();
    sliding_ = true;
    start_ticks();
}

std::tuple<double, double, double> Carousel::snapshot_outline() const {
    return {sel_float_, outline_w_, outline_h_};
}

double Carousel::compute_tug() const {
    if (config_.row_parallax == 0.0 || cards_.empty()) return 0.0;
    const double tugged = apply_rubber_band(progress_ * config_.row_parallax, start_index_,
                                            static_cast<int>(cards_.size()), config_.rubber_band);
    return std::max(-0.5, std::min(0.5, tugged));
}

double Carousel::row_origin_now() const {
    const double origin =
        overlay::row_origin(static_cast<int>(cards_.size()), visible_, screen_width_, slot_w_, gap_,
                            view_);
    return origin + tug_ * pitch_;
}

std::pair<double, double> Carousel::card_position(double index, const Card& card) const {
    const double centre =
        overlay::slot_centre_x(index, row_origin_now(), pitch_, slot_w_);
    return {centre - card.width / 2.0, row_top_ + (row_h_ - card.height) / 2.0};
}

std::vector<std::tuple<double, double, double, double>> Carousel::card_rects() const {
    std::vector<std::tuple<double, double, double, double>> rects;
    for (std::size_t index = 0; index < cards_.size(); ++index) {
        const auto [x, y] = card_position(static_cast<double>(index), cards_[index]);
        rects.emplace_back(x, y, static_cast<double>(cards_[index].width),
                           static_cast<double>(cards_[index].height));
    }
    return rects;
}

void Carousel::stub_layout(int width, int height, int count) {
    screen_width_ = width;
    screen_height_ = height;
    layout();
    cards_.clear();
    for (int index = 0; index < count; ++index) {
        Card card;
        card.width = 1440;
        card.height = 900;
        const auto [w, h] = card_size(1440, 900, slot_w_, row_h_);
        card.width = w;
        card.height = h;
        cards_.push_back(card);
    }
    selection_ = 0;
    sel_float_ = 0.0;
    outline_w_ = cards_.empty() ? 0.0 : cards_.front().width;
    outline_h_ = cards_.empty() ? 0.0 : cards_.front().height;
    progress_ = 0.0;
    start_index_ = 0;
    view_ = view_from_ = view_to_ = 0.0;
    tug_ = 0.0;
}

void Carousel::stub_select(int selection) {
    selection_ = selection;
    sel_float_ = static_cast<double>(selection);
    view_ = view_to_ = overlay::view_target(selection, static_cast<int>(cards_.size()), visible_);
}

std::tuple<double, double, double, double> Carousel::outline_rect() const {
    const double gap = config_.highlight_gap_px;
    const double width = outline_w_;
    const double height = outline_h_;
    const double centre =
        overlay::slot_centre_x(sel_float_, row_origin_now(), pitch_, slot_w_);
    const double x = centre - width / 2.0;
    const double y = row_top_ + (row_h_ - height) / 2.0;
    return {x - gap, y - gap, width + 2.0 * gap, height + 2.0 * gap};
}

std::optional<Card> Carousel::current() const {
    if (selection_ < 0 || selection_ >= static_cast<int>(cards_.size())) return std::nullopt;
    return cards_[static_cast<std::size_t>(selection_)];
}

void Carousel::settle(int steps, std::function<void()> on_commit, std::function<void()> on_done) {
    on_done_ = std::move(on_done);
    if (state_ == "idle") {
        if (on_commit) on_commit();
        finish_fade();
        return;
    }
    if (steps && on_commit) {
        try {
            on_commit();
        } catch (const std::exception& exc) {  // a failed activation must not strand us
            log_line(std::string("wintab: commit failed: ") + exc.what());
        }
    }
    begin_fade();
}

void Carousel::cancel(std::function<void()> on_done) {
    on_done_ = std::move(on_done);
    if (state_ == "idle") {
        finish_fade();
        return;
    }
    begin_fade();
}

void Carousel::abort(std::function<void()> on_done) {
    on_done_ = nullptr;
    sliding_ = false;
    progress_ = 0.0;
    state_ = "idle";
    cards_.clear();
    stop_ticks();
    if (window_) {
        gtk_widget_set_opacity(window_, 0.0);
        release_on_top();
        gtk_widget_queue_draw(window_);
    }
    if (on_done) on_done();
}

// --- animation ------------------------------------------------------------

void Carousel::begin_fade() {
    fade_from_ = 1.0;
    fade_t0_ = now_seconds();
    state_ = "fade";
    start_ticks();
}

void Carousel::start_ticks() {
    if (ticks_ || !window_) return;
    ticks_ = gtk_widget_add_tick_callback(window_, &Carousel::on_tick_cb, this, nullptr);
}

void Carousel::stop_ticks() {
    if (!ticks_) return;
    const guint tick = ticks_;
    ticks_ = 0;
    if (window_) gtk_widget_remove_tick_callback(window_, tick);
}

gboolean Carousel::on_tick_cb(GtkWidget*, GdkFrameClock*, gpointer data) {
    return static_cast<Carousel*>(data)->on_tick() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

bool Carousel::advance_slide(double now) {
    if (!sliding_) return false;
    const double total = std::max(1e-4, config_.highlight_ms / 1000.0);
    const double t = (now - slide_t0_) / total;
    const double eased = overlay::ease_out(t);
    sel_float_ = overlay::lerp(std::get<0>(outline_from_), std::get<0>(outline_to_), eased);
    outline_w_ = overlay::lerp(std::get<1>(outline_from_), std::get<1>(outline_to_), eased);
    outline_h_ = overlay::lerp(std::get<2>(outline_from_), std::get<2>(outline_to_), eased);
    view_ = overlay::lerp(view_from_, view_to_, eased);
    if (t >= 1.0) {
        sel_float_ = std::get<0>(outline_to_);
        outline_w_ = std::get<1>(outline_to_);
        outline_h_ = std::get<2>(outline_to_);
        view_ = view_to_;
        sliding_ = false;
    }
    return true;
}

bool Carousel::on_tick() {
    const double now = now_seconds();
    bool dirty = advance_slide(now);

    if (state_ == "fade") {
        const double total = std::max(1e-4, config_.fade_ms / 1000.0);
        const double t = (now - fade_t0_) / total;
        if (window_) {
            gtk_widget_set_opacity(window_, std::max(0.0, fade_from_ * (1.0 - std::min(1.0, t))));
        }
        dirty = true;
        if (t >= 1.0) {
            finish_fade();
            ticks_ = 0;  // returning FALSE below removes the source
            return false;
        }
    }

    if (dirty && window_) gtk_widget_queue_draw(window_);
    if (!sliding_ && state_ != "fade") {
        // Nothing is moving: stop ticking rather than redrawing forever.
        ticks_ = 0;
        return false;
    }
    return true;
}

void Carousel::finish_fade() {
    state_ = "idle";
    sliding_ = false;
    cards_.clear();
    if (window_) {
        gtk_widget_set_opacity(window_, 0.0);
        release_on_top();
        gtk_widget_queue_draw(window_);
    }
    stop_ticks();
    auto callback = std::move(on_done_);
    on_done_ = nullptr;
    if (callback) {
        try {
            callback();
        } catch (const std::exception& exc) {
            log_line(std::string("wintab: overlay callback failed: ") + exc.what());
        }
    }
}

// --- drawing --------------------------------------------------------------

void Carousel::on_destroy_cb(GtkWidget*, gpointer data) {
    auto* self = static_cast<Carousel*>(data);
    self->window_ = nullptr;
    self->ticks_ = 0;
    self->state_ = "idle";
}

gboolean Carousel::on_draw_cb(GtkWidget*, cairo_t* context, gpointer data) {
    return static_cast<Carousel*>(data)->on_draw(context);
}

bool Carousel::on_draw(cairo_t* context) {
    const double started = now_seconds();
    cairo_save(context);
    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    render(context);
    cairo_restore(context);
    track_fps(started);
    return false;
}

void Carousel::render(cairo_t* context) {
    if (state_ == "idle" || cards_.empty() || screen_width_ <= 0) return;

    Image backdrop;
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        backdrop = backdrop_;
    }
    if (backdrop) {
        cairo_set_source_surface(context, backdrop.surface(), 0, 0);
        cairo_paint(context);
    } else {
        cairo_set_source_rgb(context, 0.06, 0.06, 0.08);
        cairo_paint(context);
    }

    if (config_.band_alpha > 0) {
        cairo_set_source_rgba(context, 1.0, 1.0, 1.0, config_.band_alpha);
        cairo_rectangle(context, 0, band_top_, screen_width_, band_h_);
        cairo_fill(context);
    }

    for (std::size_t index = 0; index < cards_.size(); ++index) {
        const Card& card = cards_[index];
        const auto [x, y] = card_position(static_cast<double>(index), card);
        if (x + card.width < -card.padding || x - card.padding > screen_width_) {
            continue;  // scrolled off: skip the blit entirely
        }
        cairo_surface_t* surface =
            (static_cast<int>(index) == selection_) ? card.surface.surface() : card.dim_surface.surface();
        cairo_set_source_surface(context, surface, x - card.padding, y - card.padding);
        cairo_paint(context);
        draw_caption(context, card, x, y);
    }

    draw_outline(context);
    if (config_.show_fps) draw_hud(context);
}

void Carousel::draw_caption(cairo_t* context, const Card& card, double x, double y) const {
    const double font_px = config_.caption_font_px;
    if (font_px <= 0.0 || card.label.empty()) return;
    cairo_save(context);
    // Clip to the card's own column: a long name is cut off instead of running
    // into its neighbour.
    cairo_rectangle(context, x - card.padding, y, card.width + 2.0 * card.padding,
                    card.height + font_px * 2.0);
    cairo_clip(context);
    cairo_select_font_face(context, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, font_px);
    cairo_text_extents_t extents{};
    cairo_text_extents(context, card.label.c_str(), &extents);
    const double left = x + (card.width - extents.width) / 2.0 - extents.x_bearing;
    const double baseline =
        caption_baseline(y, card.height, config_.caption_gap_px, font_px);
    cairo_move_to(context, left + 1.0, baseline + 1.0);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.6);
    cairo_show_text(context, card.label.c_str());
    cairo_move_to(context, left, baseline);
    cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 0.92);
    cairo_show_text(context, card.label.c_str());
    cairo_restore(context);
}

void Carousel::draw_outline(cairo_t* context) const {
    const auto [x, y, w, h] = outline_rect();
    const double radius = config_.card_radius + config_.highlight_gap_px;
    const auto colour = config_.highlight_color();
    cairo_save(context);
    // A faint white rim just outside, then the accent blue, like Windows.
    rounded_rect(context, x - 1.5, y - 1.5, w + 3.0, h + 3.0, radius + 1.5);
    cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 0.45);
    cairo_set_line_width(context, 1.0);
    cairo_stroke(context);
    rounded_rect(context, x, y, w, h, radius);
    cairo_set_source_rgb(context, colour[0], colour[1], colour[2]);
    cairo_set_line_width(context, config_.highlight_px);
    cairo_stroke(context);
    cairo_restore(context);
}

void Carousel::draw_hud(cairo_t* context) const {
    cairo_save(context);
    cairo_select_font_face(context, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 18.0);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 0.6);
    cairo_rectangle(context, 12, 12, 380, 34);
    cairo_fill(context);
    cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 0.95);
    cairo_move_to(context, 22, 36);
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "wintab %s sel=%d/%d p=%+.2f %4.0ffps %4.1fms",
                  state_.c_str(), selection_ + 1, static_cast<int>(cards_.size()), progress_, fps_,
                  frame_ms_);
    cairo_show_text(context, buffer);
    cairo_restore(context);
}

void Carousel::track_fps(double started) {
    frame_ms_ = (now_seconds() - started) * 1000.0;
    frame_max_ms_ = std::max(frame_max_ms_, frame_ms_);
    frames_ += 1;
    draws_ += 1;
    const double now = now_seconds();
    if (last_frame_at_ == 0.0) {
        last_frame_at_ = now;
    } else if (now - last_frame_at_ >= 0.5) {
        fps_ = frames_ / (now - last_frame_at_);
        frames_ = 0;
        last_frame_at_ = now;
    }
}

std::string Carousel::stats_text() const {
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "{'state': '%s', 'cards': %d, 'selection': %d, 'view': %.3f, "
                  "'progress': %.3f, 'frame_ms': %.1f, 'frame_max_ms': %.1f, 'fps': %.1f, "
                  "'draws': %d, 'backdrop': %s}",
                  state_.c_str(), static_cast<int>(cards_.size()), selection_, view_, progress_,
                  frame_ms_, frame_max_ms_, fps_, draws_, backdrop_ ? "True" : "False");
    return buffer;
}

}  // namespace wintab
