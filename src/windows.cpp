#include "windows.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>

#include "log.hpp"

namespace wintab {
namespace {

double monotonic_now() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string lower_ascii(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

bool contained(const std::vector<WindowId>& list, WindowId xid) {
    return std::find(list.begin(), list.end(), xid) != list.end();
}

}  // namespace

std::string WinInfo::key() const {
    if (!wm_class.empty()) return wm_class;
    if (pid) return "pid:" + std::to_string(*pid);
    return "xid:" + std::to_string(xid);
}

// ---------------------------------------------------------------------------
// Pure model
// ---------------------------------------------------------------------------

std::vector<WinInfo> usable_windows(const std::vector<WinInfo>& windows, int current_desktop,
                                    std::optional<int> our_pid) {
    std::vector<WinInfo> keep;
    for (const WinInfo& win : windows) {
        if (win.desktop_window || win.dock || win.skip_taskbar || !win.normal) continue;
        if (win.our_own || (our_pid && win.pid && *win.pid == *our_pid)) continue;
        if (win.desktop != -1 && win.desktop != current_desktop) continue;
        keep.push_back(win);
    }
    return keep;
}

std::vector<AppEntry> build_app_list(const std::vector<WinInfo>& windows,
                                     const std::vector<WindowId>& mru, int current_desktop,
                                     bool group_by_app, std::optional<int> our_pid,
                                     const std::map<std::string, std::string>& labels) {
    std::map<WindowId, int> rank;
    for (std::size_t index = 0; index < mru.size(); ++index) {
        rank.emplace(mru[index], static_cast<int>(index));  // first mention wins
    }

    std::vector<WinInfo> wins = usable_windows(windows, current_desktop, our_pid);
    std::stable_sort(wins.begin(), wins.end(), [&rank](const WinInfo& a, const WinInfo& b) {
        const auto rank_of = [&rank](WindowId xid) {
            const auto it = rank.find(xid);
            return it == rank.end() ? kUnranked : it->second;
        };
        return rank_of(a.xid) < rank_of(b.xid);
    });

    std::vector<AppEntry> entries;
    std::map<std::string, std::size_t> positions;
    for (const WinInfo& win : wins) {
        const std::string key = group_by_app ? win.key() : "xid:" + std::to_string(win.xid);
        const auto found = positions.find(key);
        if (found == positions.end()) {
            std::string label;
            const auto labelled = labels.find(key);
            if (labelled != labels.end()) label = labelled->second;
            if (label.empty()) label = pretty_label(win.wm_class.empty() ? win.title : win.wm_class);
            positions.emplace(key, entries.size());
            entries.push_back(AppEntry{key, label, {}});
            entries.back().windows.push_back(WindowRef{win.xid, win.title, win.rect(), win.minimized});
        } else {
            entries[found->second].windows.push_back(
                WindowRef{win.xid, win.title, win.rect(), win.minimized});
        }
    }
    return entries;
}

int entry_index(const std::vector<AppEntry>& entries, std::optional<WindowId> xid) {
    if (!xid) return 0;
    for (std::size_t position = 0; position < entries.size(); ++position) {
        for (const WindowRef& ref : entries[position].windows) {
            if (ref.xid == *xid) return static_cast<int>(position);
        }
    }
    return 0;
}

std::vector<WindowId> strip_priority(const std::string& mode,
                                     const std::vector<WindowId>& mru_order,
                                     const std::vector<WindowId>& mapping_order) {
    if (mode == "recent") return mru_order;
    return mapping_order;
}

std::string title_case(const std::string& raw) {
    // Python's str.title(): uppercase the first cased character of each run of
    // cased characters, lowercase the rest. Apostrophes and digits are not
    // cased, so "vlc's" -> "Vlc'S" and "3d-calc" -> "3D Calc".
    std::string out = raw;
    bool previous_cased = false;
    for (char& ch : out) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (previous_cased) {
            ch = static_cast<char>(std::tolower(uch));
        } else {
            ch = static_cast<char>(std::toupper(uch));
        }
        previous_cased = std::isalpha(uch) != 0;
    }
    return out;
}

std::string pretty_label(const std::string& raw) {
    if (raw.empty()) return "?";
    std::string name = raw;
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(dot + 1);
    std::replace(name.begin(), name.end(), '-', ' ');
    return title_case(trim(name));
}

bool rects_overlap(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

std::set<WindowId> windows_needing_own_pixels(const std::vector<WinInfo>& windows,
                                              int current_desktop, const Rect& bounds,
                                              std::optional<int> our_pid) {
    std::vector<const WinInfo*> shareable;
    for (const WinInfo& win : windows) {
        if (win.minimized || win.our_own) continue;
        if (win.desktop != -1 && win.desktop != current_desktop) continue;
        if (our_pid && win.pid && *win.pid == *our_pid) continue;
        shareable.push_back(&win);
    }

    std::set<WindowId> need;
    for (std::size_t index = 0; index < shareable.size(); ++index) {
        const WinInfo& win = *shareable[index];
        const Rect rect = win.rect();
        if (rect.x < bounds.x || rect.y < bounds.y || rect.x + rect.w > bounds.x + bounds.w ||
            rect.y + rect.h > bounds.y + bounds.h) {
            // Partly off screen: a crop would come out padded, or empty.
            need.insert(win.xid);
            continue;
        }
        for (std::size_t later = index + 1; later < shareable.size(); ++later) {
            if (rects_overlap(rect, shareable[later]->rect())) {
                need.insert(win.xid);
                break;
            }
        }
    }
    return need;
}

// ---------------------------------------------------------------------------
// MruTracker
// ---------------------------------------------------------------------------

MruTracker::MruTracker(const std::vector<WindowId>& initial) {
    for (const WindowId xid : initial) {
        if (xid && !contained(order_, xid)) order_.push_back(xid);
    }
}

void MruTracker::note(WindowId xid) {
    if (!xid) return;
    order_.erase(std::remove(order_.begin(), order_.end(), xid), order_.end());
    order_.insert(order_.begin(), xid);
    if (order_.size() > 256) order_.resize(256);
}

void MruTracker::forget(WindowId xid) {
    order_.erase(std::remove(order_.begin(), order_.end(), xid), order_.end());
}

int MruTracker::rank(WindowId xid) const {
    const auto it = std::find(order_.begin(), order_.end(), xid);
    if (it == order_.end()) return kUnranked;
    return static_cast<int>(it - order_.begin());
}

void MruTracker::seed_from_stacking(const std::vector<WindowId>& stacking) {
    // Stacking is bottom-to-top, so reversing it yields most-recently-used
    // first. Known windows keep their place at the front.
    std::set<WindowId> known(order_.begin(), order_.end());
    for (auto it = stacking.rbegin(); it != stacking.rend(); ++it) {
        if (!*it || known.count(*it)) continue;
        order_.push_back(*it);
        known.insert(*it);
    }
    if (order_.size() > 256) order_.resize(256);
}

// ---------------------------------------------------------------------------
// WindowManager
// ---------------------------------------------------------------------------

WindowManager::WindowManager(const Config& config, XBackend& backend)
    : config_(config), backend_(backend), own_pid_(static_cast<int>(getpid())) {
    const std::string mode = config_.order;
    order_mode = (mode == "opened" || mode == "recent") ? mode : "recent";
    mru.seed_from_stacking(stacking_order());
}

AtomId WindowManager::atom(const std::string& name) {
    const auto it = atoms_.find(name);
    if (it != atoms_.end()) return it->second;
    const AtomId id = backend_.intern(name);
    atoms_.emplace(name, id);
    return id;
}

std::string WindowManager::atom_text_of(AtomId atom_id) {
    const auto it = atom_text_.find(atom_id);
    if (it != atom_text_.end()) return it->second;
    const std::string name = backend_.atom_name(atom_id);
    atom_text_.emplace(atom_id, name);
    return name;
}

std::set<std::string> WindowManager::atom_names(const std::vector<unsigned long>& atoms) {
    std::set<std::string> names;
    for (const unsigned long raw : atoms) {
        const std::string name = atom_text_of(static_cast<AtomId>(raw));
        if (!name.empty()) names.insert(name);
    }
    return names;
}

std::string WindowManager::text_of(const PropertyValue& value) const {
    if (!value.present()) return {};
    if (!value.bytes.empty()) return value.bytes;
    return {};
}

int WindowManager::current_desktop() {
    const PropertyValue value = backend_.property(backend_.root_window(), atom("_NET_CURRENT_DESKTOP"));
    if (value.numbers.empty()) return 0;
    return static_cast<int>(value.numbers.front());
}

int WindowManager::desktop_count() {
    const PropertyValue value =
        backend_.property(backend_.root_window(), atom("_NET_NUMBER_OF_DESKTOPS"));
    if (value.numbers.empty()) return 1;
    return std::max(1, static_cast<int>(value.numbers.front()));
}

std::vector<WindowId> WindowManager::stacking_order() {
    PropertyValue value =
        backend_.property(backend_.root_window(), atom("_NET_CLIENT_LIST_STACKING"));
    if (value.numbers.empty()) {
        value = backend_.property(backend_.root_window(), atom("_NET_CLIENT_LIST"));
    }
    std::vector<WindowId> order;
    for (const unsigned long xid : value.numbers) order.push_back(static_cast<WindowId>(xid));
    return order;
}

std::vector<WindowId> WindowManager::client_list_order() {
    const PropertyValue value = backend_.property(backend_.root_window(), atom("_NET_CLIENT_LIST"));
    if (!value.numbers.empty()) {
        std::vector<WindowId> order;
        for (const unsigned long xid : value.numbers) order.push_back(static_cast<WindowId>(xid));
        return order;
    }
    std::vector<WindowId> reversed = stacking_order();
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

std::optional<WindowId> WindowManager::active_window() {
    const PropertyValue value = backend_.property(backend_.root_window(), atom("_NET_ACTIVE_WINDOW"));
    if (value.numbers.empty()) return std::nullopt;
    const WindowId xid = static_cast<WindowId>(value.numbers.front());
    if (!xid) return std::nullopt;
    return xid;
}

std::string WindowManager::read_wm_class(WindowId xid) {
    const PropertyValue value = backend_.property(xid, atom("WM_CLASS"));
    if (value.empty()) return {};
    if (!value.bytes.empty()) {
        // WM_CLASS is "instance\0class\0"; the class half is the stable name.
        std::string best;
        std::size_t start = 0;
        while (start <= value.bytes.size()) {
            const std::size_t end = value.bytes.find('\0', start);
            const std::string part =
                value.bytes.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty()) best = part;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return lower_ascii(trim(best));
    }
    return lower_ascii(trim(text_of(value)));
}

std::string WindowManager::read_title(WindowId xid) {
    for (const char* name : {"_NET_WM_NAME", "WM_NAME"}) {
        const PropertyValue value = backend_.property(xid, atom(name));
        if (!value.empty()) {
            std::string text = value.bytes.empty() ? text_of(value) : value.bytes;
            if (!text.empty() && text.front() == '"' && text.back() == '"' && text.size() >= 2) {
                text = text.substr(1, text.size() - 2);
            }
            return trim(text);
        }
    }
    return {};
}

std::pair<std::string, std::optional<int>> WindowManager::static_facts(WindowId xid) {
    const auto it = static_.find(xid);
    if (it != static_.end()) return it->second;
    int pid_value = 0;
    const PropertyValue pid = backend_.property(xid, atom("_NET_WM_PID"));
    if (!pid.numbers.empty()) pid_value = static_cast<int>(pid.numbers.front());
    const auto facts = std::make_pair(read_wm_class(xid), pid_value ? std::optional<int>(pid_value)
                                                                   : std::nullopt);
    static_.emplace(xid, facts);
    return facts;
}

Rect WindowManager::read_frame_geometry(WindowId xid, const std::optional<Point>& position) {
    const auto geometry = backend_.geometry(xid);
    if (!geometry) return Rect{};
    const int x = position ? position->x : geometry->x;
    const int y = position ? position->y : geometry->y;

    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    const PropertyValue extents = backend_.property(xid, atom("_NET_FRAME_EXTENTS"));
    if (extents.numbers.size() >= 4) {
        left = static_cast<int>(extents.numbers[0]);
        right = static_cast<int>(extents.numbers[1]);
        top = static_cast<int>(extents.numbers[2]);
        bottom = static_cast<int>(extents.numbers[3]);
    }
    return Rect{x - left, y - top, geometry->width + left + right, geometry->height + top + bottom};
}

WindowId WindowManager::frame_of(WindowId xid) {
    const WindowId parent = backend_.parent_of(xid);
    if (!parent || parent == backend_.root_window()) return xid;
    return parent;
}

std::optional<WinInfo> WindowManager::describe(WindowId xid) {
    const auto attributes = backend_.attributes(xid);
    if (!attributes || attributes->override_redirect) return std::nullopt;

    const auto types = atom_names(backend_.property(xid, atom("_NET_WM_WINDOW_TYPE")).numbers);
    const auto states = atom_names(backend_.property(xid, atom("_NET_WM_STATE")).numbers);

    int desktop_id = -1;
    const PropertyValue desktop = backend_.property(xid, atom("_NET_WM_DESKTOP"));
    if (!desktop.numbers.empty()) {
        const unsigned long raw = desktop.numbers.front();
        desktop_id = raw == kStickyDesktop ? -1 : static_cast<int>(raw);
    }

    const auto facts = static_facts(xid);
    const std::string title = read_title(xid);
    const std::optional<Point> position = backend_.root_position(xid);
    const Rect rect = read_frame_geometry(xid, position);
    const bool placed = position.has_value();
    const WindowId frame = frame_of(xid);

    WinInfo info;
    info.xid = xid;
    info.wm_class = facts.first;
    info.pid = facts.second;
    info.title = title;
    info.x = rect.x;
    info.y = rect.y;
    info.width = rect.w;
    info.height = rect.h;
    info.desktop = desktop_id;
    info.minimized = states.count("_NET_WM_STATE_HIDDEN") > 0;
    info.skip_taskbar =
        states.count("_NET_WM_STATE_SKIP_TASKBAR") > 0 || states.count("_NET_WM_STATE_SKIP_PAGER") > 0;
    info.dock = types.count("_NET_WM_WINDOW_TYPE_DOCK") > 0;
    info.desktop_window = types.count("_NET_WM_WINDOW_TYPE_DESKTOP") > 0;
    info.our_own = info.pid && *info.pid == own_pid_;
    info.normal = types.count("_NET_WM_WINDOW_TYPE_NORMAL") > 0 || types.empty();
    info.frame = frame;
    info.placed = placed;
    return info;
}

void WindowManager::forget_missing(const std::vector<WinInfo>& windows) {
    std::set<WindowId> alive;
    for (const WinInfo& win : windows) alive.insert(win.xid);
    for (auto it = static_.begin(); it != static_.end();) {
        if (!alive.count(it->first)) {
            it = static_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<WinInfo> WindowManager::list_windows(bool force) {
    const double now = monotonic_now();
    if (!force && !windows_.empty() && now - windows_at_ < kWindowCacheSeconds) {
        return windows_;
    }
    std::vector<WinInfo> out;
    for (const WindowId xid : stacking_order()) {
        auto info = describe(xid);
        if (info) out.push_back(*info);
    }
    windows_ = out;
    windows_at_ = now;
    forget_missing(out);
    return out;
}

void WindowManager::invalidate() { windows_at_ = 0.0; }

void WindowManager::age_window_cache(double seconds) { windows_at_ = monotonic_now() - seconds; }

void WindowManager::activate(WindowId xid) {
    // A refused request must be visible: switching silently stopping working
    // once, because the timestamp was stale and nothing changed, was invisible
    // in the log otherwise (the log said "switched").
    try {
        backend_.send_activate(xid);
    } catch (const std::exception& error) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "wintab: could not activate 0x%lx: %s",
                      static_cast<unsigned long>(xid), error.what());
        log_line(buffer);
    }
}bool WindowManager::make_sticky(WindowId xid) {
    // Two requests, both required (before the map and again after it), and a
    // broken connection must be reported, never raised: an overlay left on one
    // workspace is invisible everywhere else while the gesture keeps working.
    try {
        backend_.change_cardinal(xid, atom("_NET_WM_DESKTOP"), kStickyDesktop);
        backend_.send_state_message(xid, 1, atom("_NET_WM_STATE_STICKY"), 0);
    } catch (...) {
        return false;
    }
    return true;
}
bool WindowManager::clear_attention(WindowId xid) {
    try {
        backend_.send_state_message(xid, 0, atom("_NET_WM_STATE_DEMANDS_ATTENTION"), 0);
    } catch (...) {
        return false;
    }
    return true;
}

void WindowManager::refresh_mru() { mru.seed_from_stacking(stacking_order()); }

std::pair<std::vector<AppEntry>, int> WindowManager::build() {
    const std::vector<WinInfo> windows = list_windows();
    std::vector<WindowId> priority =
        strip_priority(order_mode, mru.order(), client_list_order());
    for (const WinInfo& win : windows) priority.push_back(win.xid);
    auto entries = build_app_list(windows, priority, current_desktop(), config_.group_by_app,
                                  own_pid_);
    const int index = entry_index(entries, active_window());
    return {entries, index};
}

void WindowManager::watch_focus() {
    if (watching_) return;
    backend_.watch_property_events();
    watching_ = true;
}

void WindowManager::drain_events() {
    bool changed_focus = false;
    bool changed_desktop = false;
    PropertyEvent event;
    int processed = 0;
    while (processed++ < 128 && backend_.next_property_event(event)) {
        const std::string name = atom_text_of(event.atom);
        if (name == "_NET_ACTIVE_WINDOW") {
            if (const auto active = active_window()) {
                mru.note(*active);
                changed_focus = true;
            }
        } else if (name == "_NET_CLIENT_LIST") {
            invalidate();
            refresh_mru();
        } else if (name == "_NET_CURRENT_DESKTOP") {
            // A workspace switch is not always accompanied by a focus change,
            // and it invalidates every pixel we are holding.
            changed_desktop = true;
        }
    }
    if (changed_focus && on_focus_change) on_focus_change();
    if (changed_desktop && on_desktop_change) on_desktop_change();
}

}  // namespace wintab
