// Wiring: touchpad thread -> gesture state machine -> carousel -> window switch.
//
// Threads, exactly as in the Python build:
//   * the touchpad thread reads raw evdev frames and turns them into events and
//     never touches X11, GTK or images;
//   * the capture thread keeps the screen snapshot current and hands each fresh
//     frame to the carousel's prewarm (its own X connection, no GTK);
//   * the GTK main thread owns everything else: EWMH reads, the overlay window,
//     the animation, and the decision to switch windows.
#pragma once

#include <glib.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "capture.hpp"
#include "config.hpp"
#include "gesture.hpp"
#include "overlay.hpp"
#include "pointer.hpp"
#include "touchpad.hpp"
#include "windows.hpp"
#include "x11.hpp"

namespace wintab {

//: Other gesture handlers that would fight us for the same three fingers.
std::vector<std::string> find_conflicts();

class WinTab {
public:
    using LogFn = std::function<void(const std::string&)>;

    WinTab(Config config, bool use_overlay = true, std::optional<bool> verbose = std::nullopt,
           std::optional<std::string> record_path = std::nullopt, LogFn log = nullptr);
    ~WinTab();

    WinTab(const WinTab&) = delete;
    WinTab& operator=(const WinTab&) = delete;

    void setup();
    void start();
    void replay(const std::string& path);
    int run();
    void shutdown();

    //: Print one line per gesture event, for differential testing.
    void set_trace_events(bool enabled) { trace_events_ = enabled; }

private:
    // -- main-loop plumbing ------------------------------------------------
    void on_x_events();
    bool periodic();
    bool watchdog();
    void on_frames(const std::vector<ContactFrame>& frames);
    bool drain();
    void dispatch(const std::vector<GestureEvent>& events);
    void handle(const GestureEvent& event);

    // -- gestures ----------------------------------------------------------
    void begin();
    void end(const SwipeEnd& event);
    void cancel();
    void commit(int target);
    void confirm_switch(WindowId xid, const std::string& label);
    void after_gesture();

    // -- model / capture ---------------------------------------------------
    void refresh_rects();
    void on_focus_change();
    void on_desktop_change();
    void schedule_desktop_confirm();
    bool confirm_desktop_capture();
    bool note_desktop();
    void on_capture(int generation);
    void on_device(const std::string& message, const PadGeometry* geometry);

    // -- pointer -----------------------------------------------------------
    void grab_pointer();
    void release_pointer();

    std::string label(int index) const;
    void post_log(const std::string& message);
    std::vector<GestureEvent> feed_frames(const std::vector<ContactFrame>& frames);
    std::vector<GestureEvent> flush_swipe(double time);

    // -- GLib trampolines --------------------------------------------------
    static gboolean drain_cb(gpointer data);
    static gboolean periodic_cb(gpointer data);
    static gboolean watchdog_cb(gpointer data);
    static gboolean quit_cb(gpointer data);
    static gboolean confirm_cb(gpointer data);
    static gboolean desktop_confirm_cb(gpointer data);
    static gboolean io_cb(gint fd, GIOCondition condition, gpointer data);
    static gboolean signal_cb(gpointer data);
    static gboolean log_message_cb(gpointer data);

    Config config_;
    bool use_overlay_ = true;
    bool verbose_ = false;
    bool trace_events_ = false;
    LogFn log_;

    std::unique_ptr<XlibBackend> backend_;
    std::unique_ptr<WindowManager> wm_;
    ThreeFingerSwipe swipe_;
    std::mutex swipe_mutex_;
    std::unique_ptr<ScreenCache> cache_;
    std::unique_ptr<Carousel> overlay_;
    PointerLock pointer_lock_;

    std::vector<GestureEvent> queue_;
    std::mutex queue_mutex_;
    bool scheduled_ = false;

    GMainLoop* loop_ = nullptr;
    std::unique_ptr<TouchpadReader> reader_;
    bool replay_started_ = false;
    std::optional<std::string> record_path_;
    FILE* record_handle_ = nullptr;

    std::vector<AppEntry> entries_;
    int index_ = 0;
    std::map<WindowId, Rect> window_rects_;
    std::mutex rects_mutex_;
    int desktop_ = -1;
    int rects_desktop_ = -1;
    double rects_refreshed_at_ = 0.0;
    bool setup_done_ = false;
    guint desktop_confirm_ = 0;
    bool grabbed_ = false;
    double last_frame_at_ = 0.0;
    double logged_progress_ = 0.0;
};

}  // namespace wintab
