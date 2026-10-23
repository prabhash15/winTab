// Which windows are open in this workspace, and in what order.
//
// The filtering / grouping / ordering rules are pure functions so they can be
// tested without an X server; WindowManager is the thin layer that applies them
// to an XBackend and talks focus/activation/sticky/attention.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config.hpp"
#include "x11.hpp"

namespace wintab {

//: EWMH's "this window is on every desktop".
inline constexpr unsigned long kStickyDesktop = 0xFFFFFFFFUL;
inline constexpr int kUnranked = 1 << 30;
//: How long a window enumeration is reused for.
inline constexpr double kWindowCacheSeconds = 0.5;

struct WinInfo {
    WindowId xid = 0;
    std::string wm_class;
    std::optional<int> pid;
    std::string title;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int desktop = -1;
    bool minimized = false;
    bool skip_taskbar = false;
    bool dock = false;
    bool desktop_window = false;
    bool our_own = false;
    bool normal = true;
    //: The window the client was reparented into (its titlebar frame), or the
    //: window itself when it is a direct child of the root.
    WindowId frame = 0;
    //: False when where the window sits on screen could not be established.
    bool placed = true;

    Rect rect() const { return Rect{x, y, width, height}; }
    //: Grouping key: an app is one entry, like Windows 11.
    std::string key() const;
};

struct WindowRef {
    WindowId xid = 0;
    std::string title;
    Rect rect;
    bool minimized = false;
};

struct AppEntry {
    std::string key;
    std::string label;
    std::vector<WindowRef> windows;

    const WindowRef& top() const { return windows.front(); }
};

std::vector<WinInfo> usable_windows(const std::vector<WinInfo>& windows, int current_desktop,
                                    std::optional<int> our_pid = std::nullopt);

std::vector<AppEntry> build_app_list(const std::vector<WinInfo>& windows,
                                     const std::vector<WindowId>& mru, int current_desktop,
                                     bool group_by_app = true,
                                     std::optional<int> our_pid = std::nullopt,
                                     const std::map<std::string, std::string>& labels = {});

//: Which card the focused window belongs to (0 when it is not in the list).
int entry_index(const std::vector<AppEntry>& entries, std::optional<WindowId> xid);

//: "recent" -> MRU order; anything else -> the order windows were opened.
std::vector<WindowId> strip_priority(const std::string& mode,
                                     const std::vector<WindowId>& mru_order,
                                     const std::vector<WindowId>& mapping_order);

std::string pretty_label(const std::string& raw);
std::string title_case(const std::string& raw);

bool rects_overlap(const Rect& a, const Rect& b);

//: Windows a crop of the screen would misrepresent (occluded or off-screen).
std::set<WindowId> windows_needing_own_pixels(const std::vector<WinInfo>& windows,
                                              int current_desktop, const Rect& bounds,
                                              std::optional<int> our_pid = std::nullopt);

//: Most-recently-used window order, seeded from the stacking order and kept
//: honest by focus events.
class MruTracker {
public:
    MruTracker() = default;
    explicit MruTracker(const std::vector<WindowId>& initial);

    void note(WindowId xid);
    void forget(WindowId xid);
    int rank(WindowId xid) const;
    const std::vector<WindowId>& order() const { return order_; }
    void seed_from_stacking(const std::vector<WindowId>& stacking);

private:
    std::vector<WindowId> order_;
};

class WindowManager {
public:
    WindowManager(const Config& config, XBackend& backend);

    XBackend& backend() { return backend_; }
    int own_pid() const { return own_pid_; }
    Rect screen_geometry() { return backend_.screen_geometry(); }
    int fileno() { return backend_.fileno(); }

    std::vector<WinInfo> list_windows(bool force = false);
    void invalidate();
    //: Test support: pretend the cached enumeration is `seconds` old, so the
    //: staleness rule can be exercised without waiting for it in real time.
    void age_window_cache(double seconds);
    int current_desktop();
    int desktop_count();
    std::vector<WindowId> stacking_order();
    std::vector<WindowId> client_list_order();
    std::optional<WindowId> active_window();

    //: Current app list plus the index of the focused app.
    std::pair<std::vector<AppEntry>, int> build();

    void activate(WindowId xid);
    bool make_sticky(WindowId xid);
    bool clear_attention(WindowId xid);
    void refresh_mru();

    void watch_focus();
    void drain_events();

    std::string order_mode;
    MruTracker mru;
    std::function<void()> on_focus_change;
    std::function<void()> on_desktop_change;

private:
    AtomId atom(const std::string& name);
    std::string atom_text_of(AtomId atom);
    std::set<std::string> atom_names(const std::vector<unsigned long>& atoms);
    std::optional<WinInfo> describe(WindowId xid);
    std::pair<std::string, std::optional<int>> static_facts(WindowId xid);
    std::string read_wm_class(WindowId xid);
    std::string read_title(WindowId xid);
    Rect read_frame_geometry(WindowId xid, const std::optional<Point>& position);
    WindowId frame_of(WindowId xid);
    void forget_missing(const std::vector<WinInfo>& windows);
    std::string text_of(const PropertyValue& value) const;

    Config config_;
    XBackend& backend_;
    int own_pid_ = 0;
    std::map<std::string, AtomId> atoms_;
    std::map<AtomId, std::string> atom_text_;
    std::map<WindowId, std::pair<std::string, std::optional<int>>> static_;
    std::vector<WinInfo> windows_;
    double windows_at_ = 0.0;
    bool watching_ = false;
};

}  // namespace wintab
