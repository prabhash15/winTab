// Screen snapshots and the dimmed, blurred backdrop behind the carousel.
//
// Both are far too slow to produce during a swipe, so a worker thread keeps
// them warm: DAMAGE tells it when the screen changed, the settle timer waits
// for the desktop to hold still, then it grabs the root window, reads each
// window's own pixels through the compositor, and builds the next card set.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "image.hpp"
#include "windows.hpp"
#include "x11.hpp"

namespace wintab {

struct WindowTarget {
    WindowId xid = 0;
    //: The window the compositor actually redirects (the titlebar frame).
    WindowId frame = 0;
    Rect rect;
};

struct Snapshot {
    Image plain;
    Image backdrop;
    std::map<WindowId, Image> windows;
    int desktop = -1;
};

//: The desktop a grab belongs to: what the server said, else what we asked.
int stamped_desktop(int observed, int requested);

namespace detail {
//: Shared-memory staging for the grabs; defined in capture.cpp.
struct Shm;
}  // namespace detail

struct CapturePlan {
    //: Every window the snapshot should read through the compositor.
    std::vector<WindowTarget> targets;
    //: The windows a crop of the full-screen grab would represent correctly.
    std::map<WindowId, Rect> crops;
    int desktop = 0;
};

//: What the next snapshot should read, and where a crop is safe.
//:
//: Every placed window on this workspace is asked for its own pixels (that is
//: the reliable path); a crop is offered only where nothing else overlaps the
//: window and it sits fully on screen, which is where a crop is right anyway.
//: Kept pure so the rule can be pinned without an X server.
CapturePlan plan_capture(const std::vector<WinInfo>& everything, int desktop, const Rect& screen,
                         std::optional<int> our_pid = std::nullopt);

class ScreenCache {
public:
    using UpdateFn = std::function<void(int generation)>;

    explicit ScreenCache(const Config& config, UpdateFn on_update = nullptr);
    ~ScreenCache();

    ScreenCache(const ScreenCache&) = delete;
    ScreenCache& operator=(const ScreenCache&) = delete;

    void start();
    void stop();

    //: Called around gestures: no captures while the overlay is moving.
    void set_busy(bool busy);
    bool busy() const { return busy_; }
    double last_damage_at() const { return last_damage_at_; }

    //: Ask for a fresh snapshot (focus changed, window moved, ...).
    void refresh_soon(bool urgent = false);
    void set_targets(const std::vector<WindowTarget>& targets, int desktop);

    std::optional<Snapshot> snapshot() const;
    int generation() const;

    std::string stats_text() const;

private:
    void run();
    bool capture();
    std::map<WindowId, Image> read_windows();
    Image make_backdrop(const Image& plain);
    void arm();
    void disarm();
    void await_change();
    void settle_and_capture();
    void note_damage();
    void wake();

    Config config_;
    UpdateFn on_update_;

    mutable std::mutex mutex_;
    Image plain_;
    Image backdrop_;
    std::map<WindowId, Image> windows_;
    std::vector<WindowTarget> targets_;
    int requested_desktop_ = -1;
    int captured_desktop_ = -1;
    int capture_width_ = 0;
    int capture_height_ = 0;
    int generation_ = 0;
    int requests_ = 0;
    double captured_at_ = 0.0;
    double capture_ms_ = 0.0;
    std::string last_error_;

    std::atomic<bool> busy_{false};
    std::atomic<bool> dirty_{false};
    std::atomic<bool> urgent_{false};
    std::atomic<bool> stop_{false};
    double last_damage_at_ = 0.0;

    int wake_read_ = -1;
    int wake_write_ = -1;

    std::thread thread_;
    std::unique_ptr<detail::Shm> shm_;
    void* display_ = nullptr;  // Display*, kept out of the header
    unsigned long root_ = 0;
    unsigned long damage_id_ = 0;
    int damage_event_base_ = 0;
    bool damage_ok_ = false;
};

}  // namespace wintab
