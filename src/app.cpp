#include "app.hpp"

#include <gio/gio.h>
#include <glib-unix.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "log.hpp"

namespace wintab {
namespace {

constexpr int kTouchpadRetryMs = 5000;   // how often the window list is re-read while idle
constexpr int kWatchdogMs = 250;         // safety net for a stuck gesture
constexpr double kQuietSeconds = 0.6;    // no frames this long mid-swipe = release
constexpr int kConfirmMs = 250;          // how long to give the WM before checking
constexpr int kDesktopSettleMs = 700;    // second grab after a workspace switch

double monotonic_now() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

bool process_running(const std::string& name) {
    std::error_code error;
    std::filesystem::directory_iterator entry("/proc", error);
    if (error) return false;
    for (const auto& item : entry) {
        const std::string filename = item.path().filename().string();
        if (filename.empty() ||
            !std::all_of(filename.begin(), filename.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        std::ifstream handle(item.path() / "cmdline", std::ios::binary);
        if (!handle) continue;
        const std::string command((std::istreambuf_iterator<char>(handle)),
                                  std::istreambuf_iterator<char>());
        if (command.find(name) != std::string::npos) return true;
    }
    return false;
}

//: True/False, or nullopt when Cinnamon's gesture schema is not present.
std::optional<bool> cinnamon_gestures_enabled() {
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    if (!source) return std::nullopt;
    GSettingsSchema* schema =
        g_settings_schema_source_lookup(source, "org.cinnamon.gestures", TRUE);
    if (!schema) return std::nullopt;
    g_settings_schema_unref(schema);
    GSettings* settings = g_settings_new("org.cinnamon.gestures");
    if (!settings) return std::nullopt;
    const gboolean enabled = g_settings_get_boolean(settings, "enabled");
    g_object_unref(settings);
    return enabled != FALSE;
}

struct ConfirmData {
    WinTab* self = nullptr;
    WindowId xid = 0;
    std::string label;
};

struct Message {
    std::string text;
};

void delete_confirm(gpointer data) { delete static_cast<ConfirmData*>(data); }
void delete_message(gpointer data) { delete static_cast<Message*>(data); }

}  // namespace

std::vector<std::string> find_conflicts() {
    std::vector<std::string> warnings;

    if (process_running("touchegg")) {
        warnings.push_back(
            "Touchegg is running and already maps three-finger left/right to\n"
            "    TILE_WINDOW. Disable it (needs sudo):\n"
            "        sudo systemctl disable --now touchegg\n"
            "    or keep it for other gestures by writing a user config at\n"
            "        ~/.config/touchegg/touchegg.conf\n"
            "    that leaves out the three-finger left/right actions.");
    }

    if (cinnamon_gestures_enabled().value_or(false)) {
        warnings.push_back(
            "Cinnamon's own gestures are enabled and map three-finger\n"
            "    left/right to workspace switching. Turn them off:\n"
            "        dconf write /org/cinnamon/gestures/enabled false\n"
            "    Prefer gsettings? Use /usr/bin/gsettings explicitly: a gsettings\n"
            "    from Homebrew writes to a keyfile that Cinnamon never reads.");
    }

    return warnings;
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

WinTab::WinTab(Config config, bool use_overlay, std::optional<bool> verbose,
               std::optional<std::string> record_path, LogFn log)
    : config_(std::move(config)),
      use_overlay_(use_overlay),
      verbose_(verbose.value_or(config_.verbose)),
      log_(log ? std::move(log) : LogFn(log_line)),
      swipe_(GestureParams::from_config(config_)),
      record_path_(std::move(record_path)) {
    backend_ = std::make_unique<XlibBackend>();
    wm_ = std::make_unique<WindowManager>(config_, *backend_);
    wm_->on_focus_change = [this] { on_focus_change(); };
    wm_->on_desktop_change = [this] { on_desktop_change(); };

    if (use_overlay_) {
        cache_ = std::make_unique<ScreenCache>(config_, [this](int generation) {
            on_capture(generation);
        });
        overlay_ = std::make_unique<Carousel>(
            config_, [this](WindowId xid) { wm_->make_sticky(xid); },
            [this](WindowId xid) { wm_->clear_attention(xid); });
    }
}

WinTab::~WinTab() { shutdown(); }

// ---------------------------------------------------------------------------
// setup and run
// ---------------------------------------------------------------------------

void WinTab::setup() {
    if (setup_done_) return;  // twice would mean two capture threads, one display
    setup_done_ = true;
    gtk_init(nullptr, nullptr);
    install_x_error_handler();

    if (overlay_) {
        overlay_->ensure_window();
        refresh_rects();
        cache_->start();
    }
    wm_->watch_focus();
    g_unix_fd_add(wm_->fileno(), G_IO_IN, &WinTab::io_cb, this);
    g_timeout_add(kTouchpadRetryMs, &WinTab::periodic_cb, this);
    g_timeout_add(kWatchdogMs, &WinTab::watchdog_cb, this);
}

void WinTab::start() {
    if (record_path_) {
        record_handle_ = std::fopen(record_path_->c_str(), "a");
        if (!record_handle_) {
            log_("wintab: cannot record to " + *record_path_ + ": " + std::strerror(errno));
        }
    }
    reader_ = std::make_unique<TouchpadReader>(
        [this](const std::vector<ContactFrame>& frames) { on_frames(frames); },
        config_.touchpad_name,
        [this](const std::string& message, const PadGeometry* geometry) {
            on_device(message, geometry);
        });
    reader_->start();
}

void WinTab::replay(const std::string& path) {
    replay_started_ = true;
    std::thread([this, path] {
        std::ifstream handle(path, std::ios::binary);
        if (!handle) {
            post_log("wintab: cannot replay " + path + ": cannot open file");
            g_idle_add(&WinTab::quit_cb, this);
            return;
        }
        replay_stream(handle, 1.0, [this](const ContactFrame& frame) {
            on_frames(std::vector<ContactFrame>{frame});
        });
        g_idle_add(&WinTab::quit_cb, this);
    }).detach();
}

int WinTab::run() {
    setup();
    if (!replay_started_) start();
    loop_ = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGINT, &WinTab::signal_cb, this);
    g_unix_signal_add(SIGTERM, &WinTab::signal_cb, this);
    log_("wintab: three-finger swipe left/right to switch apps (ctrl-c to stop)");
    g_main_loop_run(loop_);
    shutdown();
    return 0;
}

void WinTab::shutdown() {
    release_pointer();
    if (reader_) {
        reader_->stop();
        reader_.reset();
    }
    if (cache_) cache_->stop();
    {
        std::lock_guard<std::mutex> lock(swipe_mutex_);
        swipe_.flush(monotonic_now());
    }
    if (overlay_) {
        overlay_->abort();
        overlay_->close();
    }
    if (record_handle_) {
        std::fclose(record_handle_);
        record_handle_ = nullptr;
    }
    if (loop_) {
        GMainLoop* loop = loop_;
        loop_ = nullptr;
        g_main_loop_unref(loop);
    }
}

gboolean WinTab::signal_cb(gpointer data) {
    auto* self = static_cast<WinTab*>(data);
    if (self->loop_) g_main_loop_quit(self->loop_);
    return G_SOURCE_CONTINUE;
}

gboolean WinTab::quit_cb(gpointer data) {
    auto* self = static_cast<WinTab*>(data);
    if (self->loop_) g_main_loop_quit(self->loop_);
    return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// main-loop plumbing
// ---------------------------------------------------------------------------

gboolean WinTab::io_cb(gint, GIOCondition, gpointer data) {
    static_cast<WinTab*>(data)->on_x_events();
    return G_SOURCE_CONTINUE;
}

void WinTab::on_x_events() { wm_->drain_events(); }

gboolean WinTab::periodic_cb(gpointer data) {
    static_cast<WinTab*>(data)->periodic();
    return G_SOURCE_CONTINUE;
}

bool WinTab::periodic() {
    // Re-reading the window list is a round trip per property per window, and a
    // stall like that on the first frames of a swipe is what a stutter is. The
    // capture thread already knows when the screen last changed.
    if (entries_.empty()) {
        if (!cache_ || cache_->last_damage_at() > rects_refreshed_at_) refresh_rects();
    }
    return true;
}

gboolean WinTab::watchdog_cb(gpointer data) {
    static_cast<WinTab*>(data)->watchdog();
    return G_SOURCE_CONTINUE;
}

bool WinTab::watchdog() {
    const double now = monotonic_now();
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(swipe_mutex_);
        active = swipe_.active();
    }
    if (active && last_frame_at_ > 0.0 && now - last_frame_at_ > kQuietSeconds) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer),
                      "wintab: no frames for %.1fs mid-swipe; treating it as a release",
                      now - last_frame_at_);
        log_(buffer);
        dispatch(flush_swipe(now));
    }

    const bool busy = [&] {
        std::lock_guard<std::mutex> lock(swipe_mutex_);
        return swipe_.active();
    }() || (overlay_ && overlay_->visible());
    if (grabbed_ && !busy) release_pointer();
    return true;
}

std::vector<GestureEvent> WinTab::feed_frames(const std::vector<ContactFrame>& frames) {
    std::vector<GestureEvent> events;
    std::lock_guard<std::mutex> lock(swipe_mutex_);
    for (const ContactFrame& frame : frames) swipe_.feed(frame, events);
    return events;
}

std::vector<GestureEvent> WinTab::flush_swipe(double time) {
    std::vector<GestureEvent> events;
    std::lock_guard<std::mutex> lock(swipe_mutex_);
    swipe_.flush(time, events);
    return events;
}

void WinTab::on_frames(const std::vector<ContactFrame>& frames) {
    last_frame_at_ = monotonic_now();
    if (record_handle_) {
        const std::string text = format_frames(frames) + "\n";
        std::fwrite(text.data(), 1, text.size(), record_handle_);
        std::fflush(record_handle_);
    }

    auto events = feed_frames(frames);
    if (events.empty()) return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.insert(queue_.end(), events.begin(), events.end());
        if (scheduled_) return;
        scheduled_ = true;
    }
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &WinTab::drain_cb, this, nullptr);
}

gboolean WinTab::drain_cb(gpointer data) {
    static_cast<WinTab*>(data)->drain();
    return G_SOURCE_REMOVE;
}

bool WinTab::drain() {
    std::vector<GestureEvent> events;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        scheduled_ = false;
        events = std::move(queue_);
        queue_.clear();
    }
    dispatch(events);
    return false;
}

void WinTab::dispatch(const std::vector<GestureEvent>& events) {
    for (const GestureEvent& event : events) {
        if (trace_events_) {
            char buffer[160];
            std::visit(
                [&](const auto& value) {
                    using Type = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Type, SwipeBegin>) {
                        std::snprintf(buffer, sizeof(buffer), "B %.17g", value.time);
                    } else if constexpr (std::is_same_v<Type, SwipeUpdate>) {
                        std::snprintf(buffer, sizeof(buffer), "U %.17g %d %.17g", value.progress,
                                      value.steps, value.time);
                    } else if constexpr (std::is_same_v<Type, SwipeEnd>) {
                        std::snprintf(buffer, sizeof(buffer), "E %.17g %.17g %d %.17g",
                                      value.progress, value.velocity, value.steps, value.time);
                    } else if constexpr (std::is_same_v<Type, SwipeCancel>) {
                        std::snprintf(buffer, sizeof(buffer), "C %s %.17g", value.reason.c_str(),
                                      value.time);
                    } else {
                        std::snprintf(buffer, sizeof(buffer), "T %.17g", value.time);
                    }
                },
                event);
            log_(buffer);
        }
        try {
            handle(event);
        } catch (const std::exception& exc) {  // one bad frame must not kill the app
            log_(std::string("wintab: gesture handling failed: ") + exc.what());
        }
    }
}

// ---------------------------------------------------------------------------
// gestures
// ---------------------------------------------------------------------------

void WinTab::handle(const GestureEvent& event) {
    std::visit(
        [this](const auto& value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, SwipeBegin>) {
                begin();
            } else if constexpr (std::is_same_v<Type, SwipeUpdate>) {
                if (overlay_) {
                    // The engine's own selection goes along, so the outlined
                    // card and the card that opens cannot drift apart.
                    overlay_->set_progress(value.progress, value.steps);
                }
                if (verbose_ && std::abs(value.progress - logged_progress_) >= 0.01) {
                    char buffer[96];
                    std::snprintf(buffer, sizeof(buffer), "wintab:   dragging p=%+.3f",
                                  value.progress);
                    log_(buffer);
                    logged_progress_ = value.progress;
                }
            } else if constexpr (std::is_same_v<Type, SwipeEnd>) {
                end(value);
            } else if constexpr (std::is_same_v<Type, SwipeCancel>) {
                log_("wintab: cancelled (" + value.reason + ")");
                cancel();
            } else {
                if (verbose_) log_("wintab: three-finger tap (nothing bound)");
            }
        },
        event);
}

void WinTab::begin() {
    logged_progress_ = 0.0;
    refresh_rects();
    auto built = wm_->build();
    entries_ = std::move(built.first);
    index_ = built.second;

    if (entries_.size() <= 1) {
        log_("wintab: only one window in this workspace, nothing to switch to");
        entries_.clear();
        return;
    }

    if (verbose_ && cache_) log_("wintab: cache " + cache_->stats_text());

    std::string others;
    if (verbose_) {
        for (std::size_t position = 0; position < entries_.size(); ++position) {
            if (static_cast<int>(position) == index_) continue;
            if (!others.empty()) others += " | ";
            others += label(static_cast<int>(position));
        }
    }
    std::string message = "wintab: strip open -- " + std::to_string(entries_.size()) +
                          " window(s) here, on " + label(index_);
    if (!others.empty()) message += ", others: " + others;
    log_(message);

    if (overlay_) {
        cache_->set_busy(true);
        overlay_->open(entries_, index_);
        if (overlay_->count() != static_cast<int>(entries_.size())) {
            // A card could not be built for some window, which would shift
            // every index: better to do nothing than switch to the wrong one.
            log_("wintab: could not build a card for every window, skipping this swipe");
            overlay_->abort();
            entries_.clear();
            cache_->set_busy(false);
            return;
        }
    }
    grab_pointer();
}

void WinTab::end(const SwipeEnd& event) {
    if (entries_.empty()) {
        release_pointer();
        return;
    }
    const int steps = clamp_steps(event.steps, index_, static_cast<int>(entries_.size()));
    char progress[32];
    char velocity[64];
    std::snprintf(progress, sizeof(progress), "%+.2f", event.progress);
    std::snprintf(velocity, sizeof(velocity), "%+.2f steps/s", event.velocity);

    if (steps == 0) {
        log_(std::string("wintab: release ") + progress + " (" + velocity + ") -> stay on " +
             label(index_));
        cancel();
        return;
    }

    const int target = index_ + steps;
    char step_text[16];
    std::snprintf(step_text, sizeof(step_text), "%+d", steps);
    log_(std::string("wintab: release ") + progress + " (" + velocity + ") -> " + step_text +
         " app(s) -> " + label(target));

    if (!overlay_) {
        commit(target);
        after_gesture();
        return;
    }
    overlay_->settle(
        steps, [this, target] { commit(target); }, [this] { after_gesture(); });
}

void WinTab::cancel() {
    release_pointer();
    if (!overlay_ || !overlay_->visible()) {
        after_gesture();
        return;
    }
    overlay_->cancel([this] { after_gesture(); });
}

void WinTab::commit(int target) {
    if (target < 0 || target >= static_cast<int>(entries_.size())) return;
    const AppEntry& entry = entries_[static_cast<std::size_t>(target)];
    const WindowRef top = entry.top();
    wm_->activate(top.xid);
    wm_->mru.note(top.xid);
    index_ = target;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "wintab:   switched to %s (0x%lx)", entry.label.c_str(),
                  top.xid);
    log_(buffer);

    // A refused switch used to be invisible: check, and say so.
    auto* data = new ConfirmData{this, top.xid, entry.label};
    g_timeout_add_full(G_PRIORITY_DEFAULT, kConfirmMs, &WinTab::confirm_cb, data,
                       &delete_confirm);
}

gboolean WinTab::confirm_cb(gpointer raw) {
    auto* data = static_cast<ConfirmData*>(raw);
    auto* self = data->self;
    const auto active = self->wm_->active_window();
    if (active && *active == data->xid) return G_SOURCE_REMOVE;
    char buffer[320];
    std::snprintf(buffer, sizeof(buffer),
                  "wintab: %s did not take focus (active is 0x%lx); asking again",
                  data->label.c_str(), active ? *active : 0UL);
    self->log_(buffer);
    self->wm_->activate(data->xid);
    return G_SOURCE_REMOVE;
}

void WinTab::after_gesture() {
    release_pointer();
    entries_.clear();
    if (cache_) {
        // Captures were paused for the overlay; freshen them and resume.
        cache_->set_busy(false);
        cache_->refresh_soon();
    }
    wm_->refresh_mru();
}

std::string WinTab::label(int index) const {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return "?";
    const AppEntry& entry = entries_[static_cast<std::size_t>(index)];
    if (entry.windows.size() > 1) {
        return entry.label + " +" + std::to_string(entry.windows.size() - 1);
    }
    const std::string& title = entry.windows.front().title;
    if (!title.empty()) return title.substr(0, 52);
    return entry.label;
}

// ---------------------------------------------------------------------------
// model and capture
// ---------------------------------------------------------------------------

bool WinTab::note_desktop() {
    int desktop = 0;
    try {
        desktop = wm_->current_desktop();
    } catch (...) {
        return false;
    }
    if (desktop == desktop_) return false;
    desktop_ = desktop;
    return true;
}

void WinTab::refresh_rects() {
    rects_refreshed_at_ = monotonic_now();
    std::vector<WinInfo> everything;
    int desktop = 0;
    CapturePlan plan;
    try {
        // Forced: noticing that a window moved is the point of this pass.
        everything = wm_->list_windows(true);
        desktop = wm_->current_desktop();
        // The rule for "read its own pixels" vs "crop the screen" is pure and
        // lives in plan_capture, so it is pinned by tests without an X server.
        plan = plan_capture(everything, desktop, wm_->screen_geometry(), wm_->own_pid());
    } catch (...) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(rects_mutex_);
        rects_desktop_ = desktop;
        window_rects_ = plan.crops;
    }

    if (cache_) {
        // Pixels are asked for on the capture thread, off the gesture path.
        cache_->set_targets(plan.targets, desktop);
    }
}

void WinTab::on_focus_change() {
    note_desktop();
    refresh_rects();
    if (cache_) cache_->refresh_soon();
}

void WinTab::on_desktop_change() {
    // The workspace changed: everything we are holding is from the old one.
    if (note_desktop() && overlay_) overlay_->forget_screen_cards();
    refresh_rects();
    if (cache_) {
        // Not even the settle wait: the strip must not show the old workspace.
        cache_->refresh_soon(true);
        schedule_desktop_confirm();
    }
}

void WinTab::schedule_desktop_confirm() {
    if (desktop_confirm_) g_source_remove(desktop_confirm_);
    desktop_confirm_ = g_timeout_add(kDesktopSettleMs, &WinTab::desktop_confirm_cb, this);
}

gboolean WinTab::desktop_confirm_cb(gpointer data) {
    auto* self = static_cast<WinTab*>(data);
    self->desktop_confirm_ = 0;
    if (self->cache_) self->cache_->refresh_soon(true);
    return G_SOURCE_REMOVE;
}

void WinTab::on_capture(int generation) {
    // Runs on the capture worker thread: build the carousel's pixels. Only
    // plain data is read here; no X11 or GTK call happens on this path.
    if (!overlay_ || !cache_) return;
    if (cache_->busy()) return;  // a swipe is on screen: bake nothing
    const auto snapshot = cache_->snapshot();
    if (!snapshot) return;

    std::map<WindowId, Rect> rects;
    int rects_desktop = -1;
    {
        std::lock_guard<std::mutex> lock(rects_mutex_);
        rects = window_rects_;
        rects_desktop = rects_desktop_;
    }

    try {
        overlay_->prewarm(snapshot->plain, snapshot->backdrop, generation, rects,
                          snapshot->windows,
                          // A crop is only allowed when the grab and the rects
                          // agree about which workspace this is.
                          snapshot->desktop == rects_desktop && rects_desktop >= 0,
                          [this] { return cache_ && cache_->busy(); });
    } catch (const std::exception& exc) {
        post_log(std::string("wintab: could not prepare overlay pixels: ") + exc.what());
    }
}

void WinTab::on_device(const std::string& message, const PadGeometry* geometry) {
    std::string text = "wintab: touchpad " + message;
    if (geometry) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), " [%d..%d x %d..%d]", geometry->x_min,
                      geometry->x_max, geometry->y_min, geometry->y_max);
        text += buffer;
    }
    post_log(text);
}

void WinTab::post_log(const std::string& message) {
    auto* payload = new Message{message};
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &WinTab::log_message_cb, payload, &delete_message);
}

gboolean WinTab::log_message_cb(gpointer raw) {
    auto* payload = static_cast<Message*>(raw);
    log_line(payload->text);
    return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// pointer
// ---------------------------------------------------------------------------

void WinTab::grab_pointer() {
    if (!config_.freeze_pointer || grabbed_) return;
    if (pointer_lock_.grab()) {
        grabbed_ = true;
    } else {
        log_("wintab: pointer freeze unavailable (" + pointer_lock_.error() + ")");
    }
}

void WinTab::release_pointer() {
    if (!grabbed_) return;
    grabbed_ = false;
    pointer_lock_.release();
}

}  // namespace wintab
