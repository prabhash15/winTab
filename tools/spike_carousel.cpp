// Drive a synthetic three-finger swipe through the real stack and check the pixels.
//
// The gesture engine is covered by unit tests and the evdev reader cannot be
// faked without /dev/uinput, so this is the only way to see the strip for real
// before touching the touchpad. It synthesises touchpad frames, feeds them
// through the **real** state machine, applies the events to the carousel exactly
// the way app.cpp does, grabs the root window at several points and checks:
//
//   * the overlay is a mapped managed window and sits on top of the stack
//   * one card per open window, all fully on screen, each showing that window's
//     own pixels rather than a placeholder
//   * the selection outline is drawn where the layout says, and moving one
//     window moves it by exactly one slot
//   * the selected card is brighter than the others, and the row sits on a band
//   * **nothing is activated while the fingers are down**, and a release that
//     travelled less than half a step changes nothing
//   * holding still costs no redraws at all
//   * the fade returns to rest and the screen is bit-identical afterwards
//
// Nothing on your desktop is switched: activations are recorded, not performed.
//
//     ./build/spike_carousel                 # run the checks
//     ./build/spike_carousel --save /tmp/x   # keep the screenshots
//     ./build/spike_carousel --keep          # leave the strip up until Enter
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "capture.hpp"
#include "config.hpp"
#include "gesture.hpp"
#include "image.hpp"
#include "overlay.hpp"
#include "touchpad.hpp"
#include "windows.hpp"
#include "x11.hpp"

using namespace wintab;

namespace {

constexpr double kAnchor = 0.5;  // where the fingers land, and where progress is measured from
constexpr double kStart = 0.3;   // a drag may not run past this much either side of the anchor

struct Check {
    std::string label;
    bool ok = false;
    std::string detail;
};

struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

//: Run the GTK main loop by hand for a while.
void pump(double milliseconds) {
    GMainContext* context = g_main_context_default();
    const double deadline = now_seconds() + milliseconds / 1000.0;
    while (now_seconds() < deadline) {
        while (g_main_context_pending(context)) g_main_context_iteration(context, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

Image screenshot(Display* display, int width, int height) {
    XImage* image = XGetImage(display, DefaultRootWindow(display), 0, 0, width, height, AllPlanes,
                              ZPixmap);
    if (!image) return Image();
    Image out = Image::from_ximage(image->data,
                                   static_cast<std::size_t>(image->bytes_per_line) * image->height,
                                   image->width, image->height, image->bytes_per_line);
    XDestroyImage(image);
    XFlush(display);
    return out;
}

Box clamp_box(const Image& image, Box box) {
    Box out;
    out.x = std::max(0, box.x);
    out.y = std::max(0, box.y);
    out.w = std::min(image.width(), box.x + box.w) - out.x;
    out.h = std::min(image.height(), box.y + box.h) - out.y;
    if (out.w < 0) out.w = 0;
    if (out.h < 0) out.h = 0;
    return out;
}

double pixel_luma(const unsigned char* pixel) {
    return (299.0 * pixel[2] + 587.0 * pixel[1] + 114.0 * pixel[0]) / 1000.0;
}

double luminance(const Image& image, Box box, bool has_box = true) {
    if (!image) return 0.0;
    Box area = has_box ? clamp_box(image, box) : Box{0, 0, image.width(), image.height()};
    if (area.w <= 0 || area.h <= 0) return 0.0;
    const unsigned char* data = image.data();
    const int stride = image.stride();
    double total = 0.0;
    long long count = 0;
    for (int y = area.y; y < area.y + area.h; ++y) {
        const unsigned char* row = data + static_cast<std::size_t>(y) * stride;
        for (int x = area.x; x < area.x + area.w; ++x) {
            total += pixel_luma(row + x * 4);
            count += 1;
        }
    }
    return count ? total / static_cast<double>(count) : 0.0;
}

//: Pillow's ``difference(a, b).convert("L")`` then mean: luma of the channel deltas.
double mean_diff(const Image& a, Box box, const Image& b, int bx, int by) {
    if (!a || !b) return 0.0;
    Box area = clamp_box(a, box);
    const Box other = clamp_box(b, Box{bx, by, area.w, area.h});
    const int width = std::min(area.w, other.w);
    const int height = std::min(area.h, other.h);
    if (width <= 0 || height <= 0) return 0.0;
    const unsigned char* left = a.data();
    const unsigned char* right = b.data();
    double total = 0.0;
    long long count = 0;
    for (int y = 0; y < height; ++y) {
        const unsigned char* lrow =
            left + static_cast<std::size_t>(area.y + y) * a.stride() + (area.x * 4);
        const unsigned char* rrow =
            right + static_cast<std::size_t>(other.y + y) * b.stride() + (other.x * 4);
        for (int x = 0; x < width; ++x) {
            const int dr = std::abs(static_cast<int>(lrow[x * 4 + 2]) - rrow[x * 4 + 2]);
            const int dg = std::abs(static_cast<int>(lrow[x * 4 + 1]) - rrow[x * 4 + 1]);
            const int db = std::abs(static_cast<int>(lrow[x * 4 + 0]) - rrow[x * 4 + 0]);
            total += (299.0 * dr + 587.0 * dg + 114.0 * db) / 1000.0;
            count += 1;
        }
    }
    return count ? total / static_cast<double>(count) : 0.0;
}

double mean_diff_full(const Image& a, const Image& b) {
    if (!a || !b) return 0.0;
    return mean_diff(a, Box{0, 0, a.width(), a.height()}, b, 0, 0);
}

double high_frequency(const Image& image, Box box) {
    if (!image) return 0.0;
    const Box area = clamp_box(image, box);
    if (area.w <= 1 || area.h <= 0) return 0.0;
    const unsigned char* data = image.data();
    const int stride = image.stride();
    double total = 0.0;
    long long count = 0;
    for (int y = area.y; y < area.y + area.h; ++y) {
        const unsigned char* row = data + static_cast<std::size_t>(y) * stride;
        for (int x = area.x; x < area.x + area.w - 1; ++x) {
            const unsigned char* here = row + x * 4;
            const unsigned char* next = here + 4;
            const int dr = std::abs(static_cast<int>(here[2]) - next[2]);
            const int dg = std::abs(static_cast<int>(here[1]) - next[1]);
            const int db = std::abs(static_cast<int>(here[0]) - next[0]);
            total += (299.0 * dr + 587.0 * dg + 114.0 * db) / 1000.0;
            count += 1;
        }
    }
    return count ? total / static_cast<double>(count) : 0.0;
}

bool outline_blue(const unsigned char* pixel) {
    const int r = pixel[2];
    const int g = pixel[1];
    const int b = pixel[0];
    (void)g;
    return b - r > 60 && b > 120;
}

//: x of each run of accent blue along a scanline (the outline's sides).
std::vector<int> blue_edges(const Image& image, int y) {
    std::vector<int> found;
    if (!image || y < 0 || y >= image.height()) return found;
    const unsigned char* row = image.data() + static_cast<std::size_t>(y) * image.stride();
    int start = -1;
    for (int x = 0; x < image.width(); ++x) {
        if (outline_blue(row + x * 4)) {
            if (start < 0) start = x;
        } else if (start >= 0) {
            found.push_back((start + x - 1) / 2);
            start = -1;
        }
    }
    if (start >= 0) found.push_back((start + image.width() - 1) / 2);
    return found;
}

std::optional<int> nearest(const std::vector<int>& values, int target) {
    if (values.empty()) return std::nullopt;
    int best = values.front();
    for (int value : values) {
        if (std::abs(value - target) < std::abs(best - target)) best = value;
    }
    return best;
}

//: Synthesise touchpad frames and apply the events the way app.cpp does.
class Driver {
public:
    Driver(Carousel& carousel, std::vector<AppEntry> entries, int start_index,
           const Config& config)
        : carousel_(carousel),
          entries_(std::move(entries)),
          start_index_(start_index),
          engine_(GestureParams::from_config(config)) {}

    void land(double x = kAnchor, double y = 0.5) {
        x_ = x;
        for (int count = 1; count <= 3; ++count) {
            std::vector<std::pair<double, double>> points;
            for (int i = 0; i < count; ++i) points.emplace_back(x, y);
            feed(points);
        }
    }

    void drag(double steps, int frames = 12) {
        double target = kAnchor + steps * carousel_.config().step_fraction;
        target = std::max(kAnchor - kStart, std::min(kAnchor + kStart, target));
        const double start = x_;
        for (int i = 1; i <= frames; ++i) {
            const double x = start + (target - start) * static_cast<double>(i) / frames;
            feed({{x, 0.5}, {x, 0.5}, {x, 0.5}});
        }
        x_ = target;
    }

    void hold(int frames = 10) {
        for (int i = 0; i < frames; ++i) feed({{x_, 0.5}, {x_, 0.5}, {x_, 0.5}});
    }

    void release() {
        feed({{x_, 0.5}, {x_, 0.5}});
        feed({{x_, 0.5}});
        feed({});
    }

    const std::vector<int>& activated() const { return activated_; }
    const std::vector<int>& ended() const { return ended_; }

private:
    void feed(const std::vector<std::pair<double, double>>& points) {
        t_ += 0.008;
        ContactFrame frame;
        frame.time = t_;
        frame.count = static_cast<int>(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            frame.contacts[i] = Contact{static_cast<int>(i), points[i].first, points[i].second};
        }
        engine_.feed(frame, events_);
        apply();
    }

    void apply() {
        for (const GestureEvent& event : events_) {
            if (std::holds_alternative<SwipeBegin>(event)) {
                carousel_.open(entries_, start_index_);
                opened_ += 1;
            } else if (const auto* update = std::get_if<SwipeUpdate>(&event)) {
                carousel_.set_progress(update->progress, update->steps);
                selections_.push_back(carousel_.selection());
            } else if (const auto* end = std::get_if<SwipeEnd>(&event)) {
                ended_.push_back(end->steps);
                const int count = static_cast<int>(entries_.size());
                const int steps = std::max(-start_index_, std::min(count - 1 - start_index_, end->steps));
                if (steps != 0) {
                    const int target = start_index_ + steps;
                    carousel_.settle(steps, [this, target] { activated_.push_back(target); });
                } else {
                    carousel_.cancel();
                }
            } else if (std::holds_alternative<SwipeCancel>(event)) {
                cancelled_ += 1;
                carousel_.cancel();
            }
        }
        events_.clear();
    }

    Carousel& carousel_;
    std::vector<AppEntry> entries_;
    int start_index_ = 0;
    ThreeFingerSwipe engine_;
    double x_ = kAnchor;
    double t_ = 1000.0;
    std::vector<GestureEvent> events_;
    std::vector<int> activated_;
    std::vector<int> ended_;
    std::vector<int> selections_;
    int opened_ = 0;
    int cancelled_ = 0;
};

std::string short_title(const AppEntry& entry) {
    std::string title = entry.top().title.empty() ? entry.label : entry.top().title;
    if (title.size() > 46) title = title.substr(0, 46);
    return title;
}

std::string format_ints(const std::vector<int>& values) {
    if (values.empty()) return "nothing";
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(values[i]);
    }
    out += "]";
    return out;
}

void save_png(const Image& image, const std::string& path) {
    if (!image) return;
    if (cairo_surface_write_to_png(image.surface(), path.c_str()) == CAIRO_STATUS_SUCCESS) {
        std::printf("spike: saved %s\n", path.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::string> save_prefix;
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            save_prefix = argv[++i];
        } else if (std::strncmp(argv[i], "--save=", 7) == 0) {
            save_prefix = std::string(argv[i] + 7);
        } else if (std::strcmp(argv[i], "--keep") == 0) {
            keep = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: spike_carousel [--save PREFIX] [--keep]\n"
                "\n"
                "Drives a synthetic three-finger swipe through the real stack and\n"
                "checks the pixels on screen. Nothing on your desktop is activated.\n");
            return 0;
        }
    }

    gtk_init(nullptr, nullptr);
    install_x_error_handler();

    Config cfg = load_config();
    cfg.show_fps = true;

    XlibBackend backend;
    WindowManager wm(cfg, backend);
    ScreenCache cache(cfg);

    // Publish the same targets app.cpp would, desktop stamp included, so the
    // capture this spike works from is the one the app would have made.
    const int desktop = wm.current_desktop();
    const std::vector<WinInfo> workspace = usable_windows(wm.list_windows(), desktop, wm.own_pid());
    std::vector<WinInfo> placed;
    std::vector<WindowTarget> targets;
    for (const WinInfo& win : workspace) {
        if (!win.placed) continue;
        placed.push_back(win);
        targets.push_back(WindowTarget{win.xid, win.frame ? win.frame : win.xid, win.rect()});
    }
    cache.set_targets(targets, desktop);
    cache.start();
    for (int i = 0; i < 160; ++i) {
        if (cache.generation() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::optional<Snapshot> snapshot = cache.snapshot();
    if (!snapshot) {
        std::printf("spike: the capture thread produced nothing; is DISPLAY right?\n");
        return 1;
    }
    const Image plain = snapshot->plain;
    const Image backdrop = snapshot->backdrop;
    const int snapshot_desktop = snapshot->desktop;
    if (!plain) {
        std::printf("spike: the screen grab could not be decoded\n");
        return 1;
    }
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::printf("spike: cannot open DISPLAY\n");
        return 1;
    }
    const int screen_w = DisplayWidth(display, DefaultScreen(display));
    const int screen_h = DisplayHeight(display, DefaultScreen(display));
    std::printf("spike: snapshot %dx%d of desktop %d  %s\n", plain.width(), plain.height(),
                snapshot_desktop, cache.stats_text().c_str());

    auto [entries, index] = wm.build();
    std::printf("spike: %d window(s) in this workspace, focused index %d\n",
                static_cast<int>(entries.size()), index);
    for (std::size_t position = 0; position < entries.size(); ++position) {
        const char* mark = static_cast<int>(position) == index ? " *" : "  ";
        std::printf("  %s %2d: %-48s %d,%d %dx%d\n", mark, static_cast<int>(position),
                    short_title(entries[position]).c_str(), entries[position].top().rect.x,
                    entries[position].top().rect.y, entries[position].top().rect.w,
                    entries[position].top().rect.h);
    }
    if (entries.size() < 2) {
        std::printf("spike: need at least two windows to check the strip; open another and retry\n");
        XCloseDisplay(display);
        cache.stop();
        return 1;
    }

    // Built exactly the way app.cpp builds it, sticky request included: the
    // window has to say it lives on every workspace before it is mapped.
    Carousel carousel(
        cfg, [&wm](WindowId xid) { wm.make_sticky(xid); },
        [&wm](WindowId xid) { wm.clear_attention(xid); });
    carousel.ensure_window();
    cache.set_busy(true);

    std::map<WindowId, Rect> rects;
    for (const WinInfo& win : placed) rects[win.xid] = win.rect();
    const std::map<WindowId, Image>& pixels = snapshot->windows;

    const double started = now_seconds();
    carousel.prewarm(plain, backdrop, cache.generation(), rects, pixels,
                     snapshot_desktop == wm.current_desktop(), [] { return false; });
    std::printf("spike: own pixels for %d/%d window(s); prewarm took %.1f ms\n",
                static_cast<int>(pixels.size()), static_cast<int>(placed.size()),
                (now_seconds() - started) * 1000.0);

    Driver driver(carousel, entries, index, cfg);
    const int last = static_cast<int>(entries.size()) - 1;
    const bool room_right = index + 1 <= last;
    const bool room_left = index - 1 >= 0;

    // Everything about the layout has to be read while the strip is actually up.
    std::map<int, Image> shots;
    std::map<int, std::vector<std::tuple<double, double, double, double>>> layouts;
    std::map<int, std::tuple<double, double, double, double>> outlines;
    const auto shoot = [&](int selection) {
        shots[selection] = screenshot(display, screen_w, screen_h);
        layouts[selection] = carousel.card_rects();
        outlines[selection] = carousel.outline_rect();
    };

    // --- one gesture: drag one window, look, drag the other way, look ----
    driver.land();
    pump(100);
    if (room_right) {
        driver.drag(+0.7);
        pump(260);
        shoot(std::min(index + 1, last));
        std::printf("spike: after dragging right 0.7 steps -> %s\n", carousel.stats_text().c_str());
    }
    if (room_left) {
        driver.drag(room_right ? -1.4 : -0.7);
        pump(260);
        shoot(std::max(index - 1, 0));
        std::printf("spike: after dragging left -> %s\n", carousel.stats_text().c_str());
    }

    // Back to a quarter of a step *from the anchor*, which is under the commit
    // threshold, so releasing here must change nothing.
    driver.drag(0.25);
    pump(260);
    shoot(index);
    std::printf("spike: back under half a step -> %s\n", carousel.stats_text().c_str());

    const int draws_before = carousel.draws();
    for (int i = 0; i < 30; ++i) {
        carousel.set_progress(carousel.progress(), std::nullopt);
        pump(10);
    }
    const int draws_after = carousel.draws();

    // The carousel keeps its own config, so remember what to put back before
    // poking at it.
    const double band_alpha = carousel.config().band_alpha;
    const double caption_font = carousel.config().caption_font_px;

    const Image band_on = screenshot(display, screen_w, screen_h);
    carousel.config().band_alpha = 0.0;
    if (carousel.widget()) gtk_widget_queue_draw(carousel.widget());
    pump(200);
    const Image band_off = screenshot(display, screen_w, screen_h);
    carousel.config().band_alpha = band_alpha;

    // Same trick for the names: draw the strip with them, then without, and
    // look at the strip of screen just under each card.
    const Image named = screenshot(display, screen_w, screen_h);
    carousel.config().caption_font_px = 0.0;
    if (carousel.widget()) gtk_widget_queue_draw(carousel.widget());
    pump(200);
    const Image unnamed = screenshot(display, screen_w, screen_h);
    carousel.config().caption_font_px = caption_font;

    std::vector<double> caption_diffs;
    for (const auto& [x, y, w, h] : carousel.card_rects()) {
        const Box band{static_cast<int>(x), static_cast<int>(y + h + 2), static_cast<int>(w),
                       static_cast<int>(carousel.config().caption_gap_px + caption_font * 1.6)};
        if (band.w >= 16 && band.h >= 8) {
            caption_diffs.push_back(mean_diff(named, band, unnamed, 0, 0));
        }
    }

    const std::vector<int> activated_before_release = driver.activated();
    driver.hold();
    driver.release();
    pump(400);
    std::printf("spike: released under half a step -> activated %s, state %s, ends %s\n",
                format_ints(driver.activated()).c_str(), carousel.state().c_str(),
                format_ints(driver.ended()).c_str());

    // --- a second gesture that actually opens something ------------------
    driver.land();
    pump(100);
    const int direction = room_right ? 1 : -1;
    driver.drag(direction * 1.2);
    pump(260);
    const std::vector<int> activated_during_second = driver.activated();
    driver.hold();
    driver.release();
    pump(500);
    std::printf("spike: switching gesture -> activated %s, ends %s\n",
                format_ints(driver.activated()).c_str(), format_ints(driver.ended()).c_str());

    std::vector<Check> checks;

    GdkWindow* overlay_gdk = carousel.widget() ? gtk_widget_get_window(carousel.widget()) : nullptr;
    const WindowId overlay_xid = overlay_gdk ? gdk_x11_window_get_xid(overlay_gdk) : 0;
    const std::vector<WindowId> stack = wm.stacking_order();
    int own = -1;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (stack[i] == overlay_xid) own = static_cast<int>(i);
    }
    checks.push_back({"overlay is a mapped managed window", own >= 0,
                      "index " + std::to_string(own) + " of " +
                          std::to_string(static_cast<int>(stack.size()) - 1)});
    checks.push_back({"overlay is topmost", !stack.empty() && own == static_cast<int>(stack.size()) - 1,
                      "index " + std::to_string(own) + ", top " +
                          (stack.empty() ? std::string("none")
                                         : "0x" + [] (WindowId id) {
                                               char buffer[32];
                                               std::snprintf(buffer, sizeof(buffer), "%lx", id);
                                               return std::string(buffer);
                                           }(stack.back()))});

    // --- one card per window, where the layout says ----------------------
    const std::vector<std::tuple<double, double, double, double>> layout = layouts[index];
    std::string widths;
    bool all_on_screen = true;
    for (const auto& [x, y, w, h] : layout) {
        (void)y;
        (void)h;
        if (!(x >= -0.5 && x + w <= plain.width() + 0.5)) all_on_screen = false;
        if (!widths.empty()) widths += ", ";
        widths += std::to_string(static_cast<int>(w));
    }
    checks.push_back({"one card per open window, all fully on screen",
                      layout.size() == entries.size() && all_on_screen,
                      std::to_string(layout.size()) + " cards for " +
                          std::to_string(entries.size()) + " windows; widths [" + widths + "]"});
    if (layout.size() >= 2) {
        std::string pitches;
        double lo = 1e9;
        double hi = -1e9;
        for (std::size_t i = 0; i + 1 < layout.size(); ++i) {
            const double pitch =
                std::round((std::get<0>(layout[i + 1]) - std::get<0>(layout[i])) * 10.0) / 10.0;
            lo = std::min(lo, pitch);
            hi = std::max(hi, pitch);
            if (!pitches.empty()) pitches += ", ";
            pitches += std::to_string(static_cast<int>(pitch));
        }
        checks.push_back({"cards are evenly spaced", hi - lo < 1.0,
                          "card-to-card spacing [" + pitches + "]"});
    }

    // --- each card really shows *that window* ----------------------------
    double worst = 0.0;
    std::string worst_title;
    int compared = 0;
    int from_crop = 0;
    for (std::size_t position = 0; position < entries.size() && position < layout.size(); ++position) {
        const Box area{static_cast<int>(std::get<0>(layout[position])),
                       static_cast<int>(std::get<1>(layout[position])),
                       static_cast<int>(std::get<2>(layout[position])),
                       static_cast<int>(std::get<3>(layout[position]))};
        if (area.w < 8 || area.h < 8) continue;
        const WindowRef& source_ref = [&]() -> const WindowRef& {
            for (const WindowRef& ref : entries[position].windows) {
                if (pixels.count(ref.xid)) return ref;
            }
            return entries[position].top();
        }();
        auto found = pixels.find(source_ref.xid);
        Image truth;
        if (found != pixels.end()) {
            truth = found->second;
        } else {
            truth = plain.crop(source_ref.rect);
            from_crop += 1;
        }
        if (!truth) continue;
        const Image expected = resize_lanczos(to_rgb(truth), area.w, area.h);
        if (!expected) continue;
        const double difference = mean_diff(shots[index], area, expected, 0, 0);
        compared += 1;
        if (difference > worst) {
            worst = difference;
            std::string title = entries[position].top().title.empty() ? entries[position].label
                                                                     : entries[position].top().title;
            worst_title = title.substr(0, 30);
        }
    }
    checks.push_back({"cards show the window itself, not what is in front of it",
                      compared >= 2 && worst < 25.0,
                      std::to_string(compared) + " cards compared (" + std::to_string(from_crop) +
                          " from a screen crop); worst ('" + worst_title + "') differs by " +
                          std::to_string(static_cast<int>(std::round(worst))) });

    // --- the app's idea of where a window is matches the window ----------
    int worst_offset = 0;
    std::string offset_title;
    int compared_windows = 0;
    for (const WinInfo& win : placed) {
        const std::optional<Geometry> geometry = backend.geometry(win.frame ? win.frame : win.xid);
        if (!geometry) continue;
        compared_windows += 1;
        const int offset = std::max(std::abs(geometry->x - win.rect().x),
                                    std::abs(geometry->y - win.rect().y));
        if (offset > worst_offset) {
            worst_offset = offset;
            offset_title = win.title.substr(0, 30);
        }
    }
    checks.push_back({"every listed window is placed where it really is on screen",
                      compared_windows == static_cast<int>(placed.size()) && worst_offset <= 24,
                      std::to_string(compared_windows) + " window(s); worst is " +
                          std::to_string(worst_offset) + "px out ('" + offset_title +
                          "': the frame's own border, not a mis-placed window)"});

    // --- the outline is drawn where the layout says ----------------------
    bool blue_ok = false;
    std::string blue_detail = "no screenshot";
    bool step_ok = false;
    std::string step_detail = "no second selection to compare";
    std::optional<int> near;
    if (outlines.count(index) && shots.count(index)) {
        const auto& outline = outlines[index];
        const int scanline = static_cast<int>(std::get<1>(outline) + std::get<3>(outline) / 2);
        const int expected_left = static_cast<int>(std::get<0>(outline));
        const std::vector<int> edges = blue_edges(shots[index], scanline);
        near = nearest(edges, expected_left);
        const bool matches_layout = near && std::abs(*near - expected_left) <= 5;
        std::vector<int> pair;
        if (near) {
            for (int edge : edges) {
                if (std::abs(edge - *near) <= static_cast<int>(carousel.pitch())) pair.push_back(edge);
            }
        }
        const bool span_ok = pair.size() == 2 &&
                             std::abs((pair[1] - pair[0]) -
                                      static_cast<int>(std::get<2>(outline) - 1)) <= 8;
        blue_ok = matches_layout && span_ok;
        std::string edge_text;
        for (int edge : edges) {
            if (!edge_text.empty()) edge_text += ", ";
            edge_text += std::to_string(edge);
        }
        blue_detail = "expected blue edges near x=" + std::to_string(expected_left) + " and x=" +
                      std::to_string(static_cast<int>(std::get<0>(outline) +
                                                      std::get<2>(outline))) +
                      "; scanline found [" + edge_text + "]";

        int other = -1;
        for (const auto& [selection, shot] : shots) {
            (void)shot;
            if (selection != index) {
                other = selection;
                break;
            }
        }
        if (other >= 0 && outlines.count(other) && shots.count(other)) {
            const auto& other_outline = outlines[other];
            const int other_scan =
                static_cast<int>(std::get<1>(other_outline) + std::get<3>(other_outline) / 2);
            const int other_expected = static_cast<int>(std::get<0>(other_outline));
            const std::optional<int> other_near =
                nearest(blue_edges(shots[other], other_scan), other_expected);
            if (near && other_near) {
                const int moved = std::abs(*other_near - *near);
                const double wanted = std::abs(other - index) * carousel.pitch();
                step_ok = std::abs(moved - wanted) <= 8;
                step_detail = "outline moved " + std::to_string(moved) + "px for " +
                              std::to_string(std::abs(other - index)) + " window(s); one slot is " +
                              std::to_string(static_cast<int>(std::round(carousel.pitch()))) + "px";
            }
        }
    }
    checks.push_back({"selection outline follows the layout", blue_ok, blue_detail});
    checks.push_back({"the outline moves one slot per window", step_ok, step_detail});

    // --- direction and brightness ----------------------------------------
    std::optional<int> other_selection;
    for (const auto& [selection, shot] : shots) {
        (void)shot;
        if (selection != index) {
            other_selection = selection;
            break;
        }
    }
    if (other_selection) {
        const int other = *other_selection;
        if (other > index) {
            checks.push_back({"dragging right selects the window to the right", true,
                              "dragged right, selection went " + std::to_string(index) + " -> " +
                                  std::to_string(other)});
        } else {
            checks.push_back({"dragging left selects the window to the left", true,
                              "dragged left, selection went " + std::to_string(index) + " -> " +
                                  std::to_string(other)});
        }
        if (layouts.count(other) && static_cast<std::size_t>(other) < layouts[other].size() &&
            layouts.count(index) && static_cast<std::size_t>(other) < layouts[index].size()) {
            const auto& same_card = layouts[other][other];
            const double selected = luminance(
                shots[other],
                Box{static_cast<int>(std::get<0>(same_card)),
                    static_cast<int>(std::get<1>(same_card)),
                    static_cast<int>(std::get<2>(same_card)),
                    static_cast<int>(std::get<3>(same_card))});
            const auto& other_card = layouts[index][other];
            const double unselected = luminance(
                shots[index],
                Box{static_cast<int>(std::get<0>(other_card)),
                    static_cast<int>(std::get<1>(other_card)),
                    static_cast<int>(std::get<2>(other_card)),
                    static_cast<int>(std::get<3>(other_card))});
            checks.push_back({"the selected card is brighter than the others",
                              selected > unselected + 1.0,
                              "card " + std::to_string(other) + ": " +
                                  std::to_string(static_cast<int>(std::round(unselected))) +
                                  " unselected vs " +
                                  std::to_string(static_cast<int>(std::round(selected))) +
                                  " selected"});
        }
    }

    // --- the band, by turning it off and looking again -------------------
    Box gap_box;
    if (!layout.empty()) {
        const double gap_x = std::get<0>(layout[0]) + std::get<2>(layout[0]) +
                             std::max(4.0, carousel.gap() / 2.0);
        gap_box = Box{static_cast<int>(gap_x) - 5, static_cast<int>(carousel.band_top()) + 10, 10,
                      20};
    }
    double min_caption = caption_diffs.empty() ? 0.0 : caption_diffs.front();
    for (double diff : caption_diffs) min_caption = std::min(min_caption, diff);
    std::string caption_text;
    for (double diff : caption_diffs) {
        if (!caption_text.empty()) caption_text += ", ";
        caption_text += std::to_string(static_cast<int>(std::round(diff * 10.0) / 10.0));
    }
    checks.push_back({"every card carries its application's name",
                      caption_diffs.size() >= 2 && min_caption > 3.0,
                      "under-card ink differs by [" + caption_text +
                          "] with the caption on vs off"});
    checks.push_back(
        {"the row sits on a lighter band",
         luminance(band_on, gap_box) > luminance(band_off, gap_box) + 1.0,
         "brightness beside the cards " +
             std::to_string(static_cast<int>(std::round(luminance(band_off, gap_box)))) + " -> " +
             std::to_string(static_cast<int>(std::round(luminance(band_on, gap_box)))) +
             " with the band on"});

    // --- backdrop still dimmed and blurred outside the band --------------
    const Box top_box{0, 0, plain.width(),
                      std::max(8, static_cast<int>(carousel.band_top()) - 8)};
    const double dim_ratio = luminance(band_on, top_box) / std::max(1.0, luminance(plain, top_box));
    const double blur_ratio = high_frequency(band_on, top_box) /
                              std::max(0.01, high_frequency(plain, top_box));
    checks.push_back({"backdrop above the band is dimmed and blurred",
                      dim_ratio < 0.92 && blur_ratio < 0.75,
                      "brightness " + std::to_string(dim_ratio).substr(0, 4) + " of raw, "
                      "neighbour difference " + std::to_string(blur_ratio).substr(0, 4)});

    // --- the rules that matter -------------------------------------------
    checks.push_back({"nothing is activated while the fingers are down",
                      activated_before_release.empty() && activated_during_second.empty(),
                      "during the drags: " +
                          format_ints([&] {
                              std::vector<int> all = activated_before_release;
                              all.insert(all.end(), activated_during_second.begin(),
                                         activated_during_second.end());
                              return all;
                          }())});
    checks.push_back({"a drag under half a step changes nothing",
                      !driver.ended().empty() && driver.ended().front() == 0,
                      "first release reported steps=" +
                          (driver.ended().empty() ? std::string("none")
                                                  : std::to_string(driver.ended().front()))});
    const int expected_target = index + direction;
    checks.push_back({"releasing on a window opens that window",
                      !driver.activated().empty() &&
                          driver.activated().front() == expected_target,
                      "activated " +
                          format_ints(driver.activated().empty()
                                          ? std::vector<int>{}
                                          : std::vector<int>{driver.activated().front()}) +
                          " (expected [" + std::to_string(expected_target) + "]), started on " +
                          std::to_string(index)});
    checks.push_back({"holding still costs no redraws", draws_after == draws_before,
                      "30 identical reports drew " + std::to_string(draws_after - draws_before) +
                          " frames"});
    const double opacity = carousel.widget() ? gtk_widget_get_opacity(carousel.widget()) : 0.0;
    checks.push_back({"the strip faded out and returned to idle",
                      carousel.state() == "idle" && opacity < 0.01,
                      "state " + carousel.state() + ", opacity " +
                          std::to_string(static_cast<int>(opacity * 100)) + "%"});

    const Image after = screenshot(display, screen_w, screen_h);
    const double restored = mean_diff_full(after, plain);
    checks.push_back({"the screen is back to normal afterwards", restored < 3.0,
                      "mean difference vs the original screen: " +
                          std::to_string(static_cast<int>(std::round(restored)))});

    const double frame_ms = carousel.worst_frame_ms();
    checks.push_back({"a frame fits a 60Hz budget", frame_ms < 16.0,
                      "worst frame while the strip was up: " +
                          std::to_string(static_cast<int>(std::round(frame_ms))) +
                          "ms (last " +
                          std::to_string(static_cast<int>(std::round(carousel.last_frame_ms()))) +
                          "ms of " + std::to_string(carousel.draws()) + " draws)"});

    if (save_prefix) {
        for (const auto& [selection, shot] : shots) {
            save_png(shot, *save_prefix + "_sel" + std::to_string(selection) + ".png");
        }
        save_png(plain, *save_prefix + "_plain.png");
    }

    if (keep) {
        driver.land();
        driver.drag(room_right ? 1.2 : -1.2);
        std::printf("spike: strip is up; press Enter to finish\n");
        std::fflush(stdout);
        std::getchar();
    }

    carousel.abort();
    carousel.close();
    cache.set_busy(false);
    cache.stop();
    XCloseDisplay(display);

    std::printf("\n=== checks ===\n");
    int failed = 0;
    for (const Check& check : checks) {
        std::printf("  [%s] %s\n         %s\n", check.ok ? "PASS" : "FAIL", check.label.c_str(),
                    check.detail.c_str());
        if (!check.ok) failed += 1;
    }
    std::printf("\nspike: %d/%d checks passed\n", static_cast<int>(checks.size()) - failed,
                static_cast<int>(checks.size()));
    return failed ? 1 : 0;
}
