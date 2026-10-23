// The carousel: a pre-mapped window showing every open window as a row of cards.
//
// Layout is pure arithmetic (so it is testable without a screen); the window
// recipe, animation and drawing live in overlay.cpp and mirror the Python
// renderer call for call.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "cards.hpp"
#include "config.hpp"
#include "image.hpp"
#include "windows.hpp"

namespace wintab::overlay {

double pitch_of(double slot_w, double gap);
int visible_slots(double screen_w, double slot_w, double gap);
double row_width(int count, double slot_w, double gap);
double view_target(int selection, int count, int visible);
double row_origin(int count, int visible, double screen_w, double slot_w, double gap, double view);
double slot_left(double index, double origin, double pitch);
double slot_centre_x(double index, double origin, double pitch, double slot_w);
double ease_out(double t);
double lerp(double a, double b, double t);

}  // namespace wintab::overlay

namespace wintab {

class Carousel {
public:
    using StickyFn = std::function<void(WindowId)>;
    using AttentionFn = std::function<void(WindowId)>;

    explicit Carousel(const Config& config, StickyFn make_sticky = nullptr,
                      AttentionFn clear_attention = nullptr);
    ~Carousel();

    Carousel(const Carousel&) = delete;
    Carousel& operator=(const Carousel&) = delete;

    GtkWidget* ensure_window();
    void close();

    // -- pixels (called from the capture thread) ---------------------------
    void prewarm(const Image& plain, const Image& backdrop, int generation,
                 const std::map<WindowId, Rect>& window_rects,
                 const std::map<WindowId, Image>& window_pixels, bool allow_crop,
                 const std::function<bool()>& cancel);

    // -- gesture API (GTK thread) ------------------------------------------
    void open(const std::vector<AppEntry>& entries, int index);
    void forget_screen_cards();
    void set_progress(double progress, std::optional<int> steps = std::nullopt);
    void settle(int steps, std::function<void()> on_commit = nullptr,
                std::function<void()> on_done = nullptr);
    void cancel(std::function<void()> on_done = nullptr);
    void abort(std::function<void()> on_done = nullptr);

    bool visible() const { return state_ != "idle"; }
    const std::string& state() const { return state_; }
    double progress() const { return progress_; }
    int selection() const { return selection_; }
    int count() const { return static_cast<int>(cards_.size()); }
    //: The capture generation the published cards were baked from. The Python
    //: build's tests assert on this; nothing in the app reads it yet.
    int generation() const;
    std::optional<Card> current() const;

    std::vector<std::tuple<double, double, double, double>> card_rects() const;
    std::tuple<double, double, double, double> outline_rect() const;

    //: Lay the strip out for a given screen with `count` stub cards, without
    //: touching a GTK window. The Python build's tests do the same thing by
    //: setting the size and calling ``_layout()``, because the geometry is the
    //: part that is easy to get subtly wrong and it needs no server.
    void stub_layout(int width, int height, int count);
    //: Point the strip at a card (and scroll to it) for the layout tests.
    void stub_select(int selection);
    //: Put the strip in the drag state, so set_progress actually moves it.
    void stub_drag() { state_ = "drag"; }
    //: Read-only views of layout state the golden tests assert on.
    double band_top() const { return band_top_; }
    double row_top() const { return row_top_; }
    double band_height() const { return band_h_; }
    double pitch() const { return pitch_; }
    double row_height() const { return row_h_; }
    int visible_slot_count() const { return visible_; }
    double tug() const { return tug_; }
    double compute_tug_now() const { return compute_tug(); }

    std::string stats_text() const;
    const Config& config() const { return config_; }
    //: The carousel keeps its own copy of the config, and the visual spike flips
    //: a value (band, captions) to prove the drawing follows it.
    Config& config() { return config_; }

    //: The GTK window, for tools that need its X id or opacity.
    GtkWidget* widget() const { return window_; }
    int draws() const { return draws_; }
    double worst_frame_ms() const { return frame_max_ms_; }
    double last_frame_ms() const { return frame_ms_; }
    double gap() const { return gap_; }

private:
    // -- window ------------------------------------------------------------
    GtkWidget* window_or_create();
    void layout();
    void assert_sticky();
    void retry_sticky_soon();
    void clear_attention_soon();
    void clear_attention_now(WindowId xid);
    void keep_on_top();
    void release_on_top();

    // -- building ----------------------------------------------------------
    std::optional<Card> card_for(const AppEntry& entry);
    Image build_backdrop(const Image& plain);

    // -- selection and animation -------------------------------------------
    void move_selection(int selection);
    std::tuple<double, double, double> snapshot_outline() const;
    double compute_tug() const;
    double row_origin_now() const;
    std::pair<double, double> card_position(double index, const Card& card) const;
    bool advance_slide(double now);
    bool on_tick();
    void start_ticks();
    void stop_ticks();
    void begin_fade();
    void finish_fade();

    // -- drawing -----------------------------------------------------------
    bool on_draw(cairo_t* context);
    void render(cairo_t* context);
    void draw_caption(cairo_t* context, const Card& card, double x, double y) const;
    void draw_outline(cairo_t* context) const;
    void draw_hud(cairo_t* context) const;
    void track_fps(double started);

    static gboolean on_draw_cb(GtkWidget* widget, cairo_t* context, gpointer data);
    static void on_destroy_cb(GtkWidget* widget, gpointer data);
    static gboolean on_tick_cb(GtkWidget* widget, GdkFrameClock* clock, gpointer data);
    static gboolean sticky_retry_cb(gpointer data);
    static gboolean attention_retry_cb(gpointer data);

    Config config_;
    StickyFn make_sticky_;
    AttentionFn clear_attention_callback_;
    GtkWidget* window_ = nullptr;

    // layout, recomputed whenever the strip opens
    int screen_width_ = 0;
    int screen_height_ = 0;
    double slot_w_ = 0.0;
    double gap_ = 0.0;
    double pitch_ = 0.0;
    int visible_ = 1;
    double row_h_ = 0.0;
    double row_top_ = 0.0;
    double band_top_ = 0.0;
    double band_h_ = 0.0;

    // the gesture
    std::vector<Card> cards_;
    int start_index_ = 0;
    int selection_ = 0;
    double progress_ = 0.0;
    std::string state_ = "idle";
    double tug_ = 0.0;

    // animated selection outline
    double sel_float_ = 0.0;
    std::tuple<double, double, double> outline_from_{0.0, 0.0, 0.0};
    std::tuple<double, double, double> outline_to_{0.0, 0.0, 0.0};
    double outline_w_ = 0.0;
    double outline_h_ = 0.0;
    double view_ = 0.0;
    double view_from_ = 0.0;
    double view_to_ = 0.0;
    double slide_t0_ = 0.0;
    bool sliding_ = false;

    // caches, all replaced wholesale
    mutable std::mutex buffers_mutex_;
    Image backdrop_;
    int backdrop_generation_ = -1;
    std::map<WindowId, Card> cards_by_xid_;
    int buffer_generation_ = -1;
    std::map<std::string, Card> placeholders_;

    // fade
    double fade_from_ = 0.0;
    double fade_t0_ = 0.0;
    std::function<void()> on_done_;

    int attention_clears_ = 0;
    int sticky_tries_ = 0;

    guint ticks_ = 0;
    int frames_ = 0;
    double frame_ms_ = 0.0;
    double frame_max_ms_ = 0.0;
    double last_frame_at_ = 0.0;
    double fps_ = 0.0;
    int draws_ = 0;
};

}  // namespace wintab
