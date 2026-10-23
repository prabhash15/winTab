#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "config.hpp"
#include "support/fake_x11.hpp"
#include "support/harness.hpp"
#include "windows.hpp"

using namespace wintab;

namespace {

WinInfo win(WindowId xid, std::string wm_class = "brave-browser", int desktop = -1) {
    WinInfo info;
    info.xid = xid;
    info.wm_class = std::move(wm_class);
    info.desktop = desktop;
    info.width = 800;
    info.height = 600;
    return info;
}

void add_window(testing::FakeBackend& backend, WindowId xid, const std::string& wm_class,
                int pid, const std::string& title, int desktop = 0, bool minimized = false,
                bool normal_type = true, WindowId frame = 0) {
    backend.set_geometry(xid, Geometry{0, 0, 800, 600});
    backend.set_root_position(xid, Point{10, 20});
    (void)frame;
    const std::string classes = std::string("instance") + '\0' + wm_class + '\0';
    backend.set_bytes(xid, "WM_CLASS", classes);
    backend.set_numbers(xid, "_NET_WM_PID", {static_cast<unsigned long>(pid)});
    backend.set_bytes(xid, "_NET_WM_NAME", title);
    backend.set_numbers(xid, "_NET_WM_DESKTOP", {static_cast<unsigned long>(desktop)});
    std::vector<unsigned long> states;
    if (minimized) states.push_back(backend.intern("_NET_WM_STATE_HIDDEN"));
    backend.set_numbers(xid, "_NET_WM_STATE", states);
    std::vector<unsigned long> types;
    if (normal_type) types.push_back(backend.intern("_NET_WM_WINDOW_TYPE_NORMAL"));
    backend.set_numbers(xid, "_NET_WM_WINDOW_TYPE", types);
}

}  // namespace

TEST_CASE(windows_only_this_workspace) {
    const std::vector<WinInfo> windows{win(1, "brave-browser", 0), win(2, "nemo", 1)};
    const auto kept = usable_windows(windows, 0);
    CHECK_EQ(kept.size(), std::size_t(1));
    CHECK_EQ(kept.front().xid, WindowId(1));
}

TEST_CASE(windows_sticky_windows_belong_everywhere) {
    const std::vector<WinInfo> windows{win(1, "brave-browser", -1)};
    CHECK_EQ(usable_windows(windows, 3).size(), std::size_t(1));
}

TEST_CASE(windows_docks_desktop_and_skip_taskbar_are_never_listed) {
    auto dock = win(1, "panel");
    dock.dock = true;
    auto desktop_window = win(2, "nemo-desktop");
    desktop_window.desktop_window = true;
    auto skip = win(3, "tooltip");
    skip.skip_taskbar = true;
    auto not_normal = win(4, "dialog");
    not_normal.normal = false;
    const std::vector<WinInfo> windows{dock, desktop_window, skip, not_normal};
    CHECK(usable_windows(windows, 0).empty());
}

TEST_CASE(windows_our_own_overlay_is_never_listed) {
    auto ours = win(1, "wintab");
    ours.our_own = true;
    auto same_pid = win(2, "other");
    same_pid.pid = 4242;
    const std::vector<WinInfo> windows{ours, same_pid};
    CHECK(usable_windows(windows, 0, 4242).empty());
}

TEST_CASE(windows_minimized_windows_stay_listed) {
    auto minimized = win(1);
    minimized.minimized = true;
    CHECK_EQ(usable_windows({minimized}, 0).size(), std::size_t(1));
}

TEST_CASE(windows_of_one_app_group_into_one_entry) {
    const std::vector<WinInfo> windows{win(1), win(2, "brave-browser"), win(3, "nemo")};
    const auto entries = build_app_list(windows, {1, 2, 3}, 0);
    CHECK_EQ(entries.size(), std::size_t(2));
    CHECK_EQ(entries.front().key, std::string("brave-browser"));
    CHECK_EQ(entries.front().windows.size(), std::size_t(2));
}

TEST_CASE(windows_per_window_mode_keeps_them_apart) {
    const std::vector<WinInfo> windows{win(1), win(2)};
    const auto entries = build_app_list(windows, {1, 2}, 0, false);
    CHECK_EQ(entries.size(), std::size_t(2));
    CHECK_EQ(entries[0].windows.front().xid, WindowId(1));
    CHECK_EQ(entries[1].windows.front().xid, WindowId(2));
}

TEST_CASE(windows_without_a_class_group_by_pid) {
    auto first = win(1, "");
    first.pid = 5;
    auto second = win(2, "");
    second.pid = 5;
    auto third = win(3, "");
    third.pid = 6;
    const auto entries = build_app_list({first, second, third}, {1, 2, 3}, 0);
    CHECK_EQ(entries.size(), std::size_t(2));
    CHECK_EQ(entries.front().key, std::string("pid:5"));
}

TEST_CASE(windows_most_recently_used_first) {
    const std::vector<WinInfo> windows{win(1, "brave-browser"), win(2, "cinnamon-settings"),
                                       win(3, "nemo")};
    const auto entries = build_app_list(windows, {3, 2, 1}, 0);
    CHECK_EQ(entries[0].key, std::string("nemo"));
    CHECK_EQ(entries[1].key, std::string("cinnamon-settings"));
    CHECK_EQ(entries[2].key, std::string("brave-browser"));
}

TEST_CASE(windows_a_group_ranks_by_its_best_window) {
    const std::vector<WinInfo> windows{win(1, "brave-browser"), win(2, "brave-browser"),
                                       win(3, "nemo")};
    const auto entries = build_app_list(windows, {2, 3, 1}, 0);
    CHECK_EQ(entries[0].key, std::string("brave-browser"));
    CHECK_EQ(entries[1].key, std::string("nemo"));
}

TEST_CASE(windows_unranked_windows_go_last) {
    const std::vector<WinInfo> windows{win(1), win(2), win(3)};
    const auto entries = build_app_list(windows, {3}, 0, false);
    CHECK_EQ(entries.front().windows.front().xid, WindowId(3));
    CHECK_EQ(entries.size(), std::size_t(3));
}

TEST_CASE(windows_first_mention_wins) {
    const std::vector<WinInfo> windows{win(1), win(2), win(3)};
    const auto entries = build_app_list(windows, {3, 1, 2, 1, 2, 3}, 0, false);
    CHECK_EQ(entries[0].windows.front().xid, WindowId(3));
    CHECK_EQ(entries[1].windows.front().xid, WindowId(1));
    CHECK_EQ(entries[2].windows.front().xid, WindowId(2));
}

TEST_CASE(windows_labels_are_readable) {
    const auto entries = build_app_list({win(1, "cinnamon-settings gestures")}, {1}, 0);
    CHECK_EQ(entries.front().label, std::string("Cinnamon Settings Gestures"));
}

TEST_CASE(windows_pretty_label) {
    CHECK_EQ(pretty_label("org.gnome.Nautilus"), std::string("Nautilus"));
    CHECK_EQ(pretty_label("firefox"), std::string("Firefox"));
    CHECK_EQ(pretty_label(""), std::string("?"));
    CHECK_EQ(pretty_label("vlc's player"), std::string("Vlc'S Player"));
    CHECK_EQ(pretty_label("3d-calc"), std::string("3D Calc"));
    CHECK_EQ(title_case("brave-browser"), std::string("Brave-Browser"));
}

TEST_CASE(windows_strip_priority_modes) {
    CHECK_EQ(strip_priority("recent", {3, 2, 1}, {1, 2, 3}), (std::vector<WindowId>{3, 2, 1}));
    CHECK_EQ(strip_priority("opened", {3, 2, 1}, {1, 2, 3}), (std::vector<WindowId>{1, 2, 3}));
    CHECK_EQ(strip_priority("sideways", {3, 2, 1}, {1, 2, 3}), (std::vector<WindowId>{1, 2, 3}));
}

TEST_CASE(windows_entry_index_falls_back_to_the_first_card) {
    const auto entries = build_app_list({win(1), win(2, "nemo")}, {1, 2}, 0);
    CHECK_EQ(entry_index(entries, 2), 1);
    CHECK_EQ(entry_index(entries, 999), 0);
    CHECK_EQ(entry_index(entries, std::nullopt), 0);
}

TEST_CASE(windows_mru_tracker) {
    MruTracker mru({1, 2, 3});
    CHECK_EQ(mru.order(), (std::vector<WindowId>{1, 2, 3}));
    mru.note(3);
    CHECK_EQ(mru.order(), (std::vector<WindowId>{3, 1, 2}));
    mru.forget(1);
    CHECK_EQ(mru.order(), (std::vector<WindowId>{3, 2}));
    CHECK_EQ(mru.rank(3), 0);
    CHECK_EQ(mru.rank(42), kUnranked);
    mru.note(0);  // bogus ids are ignored
    CHECK_EQ(mru.order().size(), std::size_t(2));
}

TEST_CASE(windows_mru_seeding_puts_the_top_window_first) {
    MruTracker mru;
    mru.seed_from_stacking({10, 20, 30});  // bottom to top
    CHECK_EQ(mru.order(), (std::vector<WindowId>{30, 20, 10}));
    mru.note(10);
    mru.seed_from_stacking({10, 20, 30});
    CHECK_EQ(mru.order().front(), WindowId(10));
}

TEST_CASE(windows_mru_is_bounded) {
    MruTracker mru;
    for (std::size_t i = 1; i <= 300; ++i) mru.note(static_cast<WindowId>(i));
    CHECK_EQ(mru.order().size(), std::size_t(256));
    CHECK_EQ(mru.order().front(), WindowId(300));
}

TEST_CASE(windows_rect_overlap) {
    CHECK(rects_overlap(Rect{0, 0, 10, 10}, Rect{5, 5, 10, 10}));
    CHECK(!rects_overlap(Rect{0, 0, 10, 10}, Rect{10, 0, 10, 10}));
    CHECK(!rects_overlap(Rect{0, 0, 10, 10}, Rect{0, 10, 10, 10}));
}

TEST_CASE(windows_a_covered_window_needs_its_own_pixels) {
    auto back = win(1, "brave-browser");
    back.x = 0;
    back.y = 0;
    back.width = 800;
    back.height = 600;
    auto front = win(2, "nemo");
    front.x = 100;
    front.y = 100;
    front.width = 400;
    front.height = 300;
    const auto need = windows_needing_own_pixels({back, front}, 0, Rect{0, 0, 1920, 1080});
    CHECK(need.count(1) == 1);
    CHECK(need.count(2) == 0);
}

TEST_CASE(windows_a_window_hanging_off_screen_needs_its_own_pixels) {
    auto window = win(1);
    window.x = -50;
    const auto need = windows_needing_own_pixels({window}, 0, Rect{0, 0, 1920, 1080});
    CHECK(need.count(1) == 1);
}

TEST_CASE(windows_other_workspaces_and_minimized_windows_cover_nothing) {
    auto back = win(1);
    auto other = win(2, "nemo", 1);
    other.x = 0;
    other.y = 0;
    other.width = 800;
    other.height = 600;
    auto minimized = win(3, "thunar");
    minimized.x = 0;
    minimized.y = 0;
    minimized.width = 800;
    minimized.height = 600;
    minimized.minimized = true;
    const auto need = windows_needing_own_pixels({back, other, minimized}, 0, Rect{0, 0, 1920, 1080});
    CHECK(need.empty());
}

// --- WindowManager against the in-memory backend ---------------------------

TEST_CASE(manager_describes_windows_from_ewmh) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1, 2});
    add_window(backend, 1, "Brave-browser", 1000, "Inbox", 0);
    add_window(backend, 2, "nemo", 2000, "Files", 3);

    Config config;
    WindowManager manager(config, backend);
    const auto windows = manager.list_windows();
    CHECK_EQ(windows.size(), std::size_t(2));
    CHECK_EQ(windows[0].wm_class, std::string("brave-browser"));
    CHECK(windows[0].pid && *windows[0].pid == 1000);
    CHECK_EQ(windows[0].title, std::string("Inbox"));
    CHECK_EQ(windows[0].desktop, 0);
    CHECK_EQ(windows[0].x, 10);
    CHECK_EQ(windows[0].y, 20);
    CHECK(windows[0].placed);
    CHECK(windows[1].desktop == 3);
}

TEST_CASE(manager_a_sticky_desktop_is_minus_one) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1});
    add_window(backend, 1, "panelish", 10, "Sticky", 0);
    backend.set_numbers(1, "_NET_WM_DESKTOP", {0xFFFFFFFFUL});
    Config config;
    WindowManager manager(config, backend);
    CHECK_EQ(manager.list_windows().front().desktop, -1);
}

TEST_CASE(manager_reads_states_and_types) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1, 2, 3});
    add_window(backend, 1, "brave", 10, "a", 0, true);
    add_window(backend, 2, "panel", 11, "b", 0, false, false);
    backend.set_numbers(2, "_NET_WM_WINDOW_TYPE", {backend.intern("_NET_WM_WINDOW_TYPE_DOCK")});
    add_window(backend, 3, "nemo-desktop", 12, "c", 0, false, false);
    backend.set_numbers(3, "_NET_WM_WINDOW_TYPE",
                        {backend.intern("_NET_WM_WINDOW_TYPE_DESKTOP")});
    Config config;
    WindowManager manager(config, backend);
    const auto windows = manager.list_windows();
    CHECK(windows[0].minimized);
    CHECK(windows[1].dock);
    CHECK(windows[2].desktop_window);
}

TEST_CASE(manager_our_own_windows_are_flagged) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1});
    add_window(backend, 1, "wintab", static_cast<int>(getpid()), "strip", 0);
    Config config;
    WindowManager manager(config, backend);
    CHECK(manager.list_windows().front().our_own);
}

TEST_CASE(manager_the_window_list_is_cached_for_half_a_second) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1});
    add_window(backend, 1, "brave", 10, "first", 0);
    Config config;
    WindowManager manager(config, backend);
    CHECK_EQ(manager.list_windows().front().title, std::string("first"));
    backend.set_bytes(1, "_NET_WM_NAME", "second");
    CHECK_EQ(manager.list_windows().front().title, std::string("first"));
    CHECK_EQ(manager.list_windows(true).front().title, std::string("second"));
    manager.invalidate();
    CHECK_EQ(manager.list_windows().front().title, std::string("second"));
}

TEST_CASE(manager_frame_is_read_through_the_parent) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {5});
    add_window(backend, 5, "brave", 10, "t", 0);
    backend.set_root(1);
    backend.set_parent(5, 77);
    Config config;
    WindowManager manager(config, backend);
    CHECK_EQ(manager.list_windows().front().frame, WindowId(77));
}

TEST_CASE(manager_an_unplaceable_window_is_not_placed) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {5});
    add_window(backend, 5, "brave", 10, "t", 0);
    backend.set_root_position(5, std::nullopt);
    Config config;
    WindowManager manager(config, backend);
    const auto windows = manager.list_windows();
    CHECK_EQ(windows.size(), std::size_t(1));
    CHECK(!windows.front().placed);
}

TEST_CASE(manager_frame_extents_widen_the_rect) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {5});
    add_window(backend, 5, "brave", 10, "t", 0);
    backend.set_numbers(5, "_NET_FRAME_EXTENTS", {2, 2, 30, 2});
    Config config;
    WindowManager manager(config, backend);
    const auto info = manager.list_windows().front();
    CHECK_EQ(info.x, 8);
    CHECK_EQ(info.y, -10);
    CHECK_EQ(info.width, 804);
    CHECK_EQ(info.height, 632);
}

TEST_CASE(manager_activation_quotes_the_current_time) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1});
    add_window(backend, 1, "brave", 10, "t", 0);
    Config config;
    WindowManager manager(config, backend);
    manager.activate(1);
    CHECK_EQ(backend.activated().size(), std::size_t(1));
    CHECK_EQ(backend.activated().front(), WindowId(1));
}

TEST_CASE(manager_sticky_writes_the_property_and_the_state_message) {
    testing::FakeBackend backend;
    Config config;
    WindowManager manager(config, backend);
    CHECK(manager.make_sticky(42));
    CHECK_EQ(backend.cardinals().size(), std::size_t(1));
    CHECK_EQ(std::get<1>(backend.cardinals().front()), std::string("_NET_WM_DESKTOP"));
    CHECK_EQ(std::get<2>(backend.cardinals().front()), 0xFFFFFFFFUL);
    CHECK_EQ(backend.state_messages().size(), std::size_t(1));
    CHECK_EQ(std::get<1>(backend.state_messages().front()), 1);  // add
    CHECK_EQ(std::get<2>(backend.state_messages().front()),
             std::string("_NET_WM_STATE_STICKY"));
}

TEST_CASE(manager_attention_is_withdrawn_with_a_remove_message) {
    testing::FakeBackend backend;
    Config config;
    WindowManager manager(config, backend);
    CHECK(manager.clear_attention(42));
    CHECK_EQ(std::get<1>(backend.state_messages().front()), 0);  // remove
    CHECK_EQ(std::get<2>(backend.state_messages().front()),
             std::string("_NET_WM_STATE_DEMANDS_ATTENTION"));
}

TEST_CASE(manager_build_orders_by_mru_and_finds_the_focused_card) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1, 2, 3});
    backend.set_root_numbers("_NET_CLIENT_LIST", {1, 2, 3});
    backend.set_root_numbers("_NET_ACTIVE_WINDOW", {2});
    backend.set_root_numbers("_NET_CURRENT_DESKTOP", {0});
    add_window(backend, 1, "brave", 10, "browser", 0);
    add_window(backend, 2, "nemo", 11, "files", 0);
    add_window(backend, 3, "thunar", 12, "other", 0);

    Config config;
    config.group_by_app = true;
    WindowManager manager(config, backend);
    manager.mru.note(2);  // the focused window leads the row
    const auto [entries, index] = manager.build();
    CHECK_EQ(entries.size(), std::size_t(3));
    CHECK_EQ(index, 0);  // nemo is the active window and the first card
    CHECK_EQ(entries.front().key, std::string("nemo"));
}

TEST_CASE(manager_drain_events_dispatches_hooks) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1});
    backend.set_root_numbers("_NET_ACTIVE_WINDOW", {1});
    add_window(backend, 1, "brave", 10, "t", 0);
    Config config;
    WindowManager manager(config, backend);
    int focus = 0;
    int desktop = 0;
    manager.on_focus_change = [&focus] { ++focus; };
    manager.on_desktop_change = [&desktop] { ++desktop; };

    backend.push_property_event("_NET_ACTIVE_WINDOW");
    backend.push_property_event("_NET_CURRENT_DESKTOP");
    backend.push_property_event("_NET_WM_NAME");  // unrelated: ignored
    manager.drain_events();
    CHECK_EQ(focus, 1);
    CHECK_EQ(desktop, 1);
    CHECK_EQ(manager.mru.order().front(), WindowId(1));
}

TEST_CASE(manager_watch_focus_selects_property_events) {
    testing::FakeBackend backend;
    Config config;
    WindowManager manager(config, backend);
    manager.watch_focus();
    CHECK(backend.watching());
}

// --- grouping and ordering, the rest of the Python suite ------------------

TEST_CASE(windows_apps_from_other_workspaces_disappear_entirely) {
    auto here = win(1, "brave-browser", 0);
    auto there = win(2, "brave-browser", 1);
    const auto entries = build_app_list({here, there}, {2, 1}, 0);
    CHECK_EQ(entries.size(), std::size_t(1));
    CHECK_EQ(entries.front().windows.size(), std::size_t(1));
    CHECK_EQ(entries.front().windows.front().xid, WindowId(1));
}

TEST_CASE(windows_windows_inside_a_group_follow_mru_too) {
    const std::vector<WinInfo> windows{win(1), win(2), win(3)};
    const auto entries = build_app_list(windows, {2, 1, 3}, 0, false);
    CHECK_EQ(entries[0].windows.front().xid, WindowId(2));
    CHECK_EQ(entries[1].windows.front().xid, WindowId(1));
    CHECK_EQ(entries[2].windows.front().xid, WindowId(3));
}

TEST_CASE(windows_row_does_not_move_when_focus_changes) {
    // Focus history in, focus history out: with "opened" the row is the same
    // before and after a switch, only the outline moves.
    const std::vector<WinInfo> windows{win(1), win(2), win(3)};
    const std::vector<WindowId> mapping{1, 2, 3};
    const auto opened = strip_priority("opened", {3, 2, 1}, mapping);
    std::vector<WindowId> before_order = opened;
    before_order.insert(before_order.end(), mapping.begin(), mapping.end());
    const auto before = build_app_list(windows, before_order, 0, false);

    const auto opened_after = strip_priority("opened", {1, 3, 2}, mapping);
    std::vector<WindowId> after_order = opened_after;
    after_order.insert(after_order.end(), mapping.begin(), mapping.end());
    const auto after = build_app_list(windows, after_order, 0, false);

    CHECK_EQ(before[0].windows.front().xid, WindowId(1));
    CHECK_EQ(before[1].windows.front().xid, WindowId(2));
    CHECK_EQ(before[2].windows.front().xid, WindowId(3));
    CHECK_EQ(entry_index(before, 3), 2);
    CHECK_EQ(entry_index(after, 1), 0);
}

TEST_CASE(windows_focused_window_is_not_pinned_to_the_end) {
    // With a stable row, focusing the first window must outline the first
    // card -- the old MRU ordering always outlined the rightmost one, so every
    // rightward swipe hit the end stop.
    const std::vector<WinInfo> windows{win(1), win(2), win(3)};
    const auto entries = build_app_list(windows, {1, 2, 3, 1, 2, 3}, 0, false);
    CHECK_EQ(entry_index(entries, 1), 0);
    CHECK_EQ(entry_index(entries, 2), 1);
    CHECK_EQ(entry_index(entries, 3), 2);
}

TEST_CASE(windows_entry_index_finds_a_window_inside_a_group) {
    auto nemo = win(1, "nemo");
    auto brave1 = win(2);
    auto brave2 = win(3);
    const auto entries = build_app_list({nemo, brave1, brave2}, {1, 2, 3}, 0);
    // Brave (2 and 3) is one card, so its second window is still card 1.
    CHECK_EQ(entry_index(entries, 3), 1);
}

// --- the enumeration cache -------------------------------------------------

TEST_CASE(manager_a_stale_cache_re_reads) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1});
    add_window(backend, 1, "brave", 10, "first", 0);
    Config config;
    WindowManager manager(config, backend);
    CHECK_EQ(manager.list_windows().front().title, std::string("first"));
    backend.set_bytes(1, "_NET_WM_NAME", "second");
    manager.age_window_cache(kWindowCacheSeconds + 1.0);
    CHECK_EQ(manager.list_windows().front().title, std::string("second"));
}

TEST_CASE(manager_the_returned_list_cannot_be_edited_by_the_caller) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1, 2});
    add_window(backend, 1, "brave", 10, "a", 0);
    add_window(backend, 2, "nemo", 11, "b", 0);
    Config config;
    WindowManager manager(config, backend);
    auto windows = manager.list_windows();
    windows.clear();
    // A second read must reuse the cache, so clearing the copy cannot touch it.
    CHECK_EQ(manager.list_windows().size(), std::size_t(2));
}

TEST_CASE(manager_a_gone_window_is_forgotten) {
    // A window that stops being a real window (turned into an override-
    // redirect surface) must vanish from the list on the next forced read,
    // rather than being kept alive by the static-facts cache.
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1, 2});
    add_window(backend, 1, "brave", 10, "a", 0);
    add_window(backend, 2, "nemo", 11, "b", 0);
    Config config;
    WindowManager manager(config, backend);
    CHECK_EQ(manager.list_windows().size(), std::size_t(2));
    backend.set_override_redirect(2, true);
    const auto windows = manager.list_windows(true);
    CHECK_EQ(windows.size(), std::size_t(1));
    CHECK_EQ(windows.front().xid, WindowId(1));
}

// --- where the window actually sits ---------------------------------------

TEST_CASE(manager_the_rect_uses_the_root_position_not_the_parent_one) {
    // Taken from the machine this was built on: a Cinnamon Settings window
    // whose client sits at (10, 40) inside a frame at (1031, 89), with a 32px
    // titlebar above it. The crop has to start at the frame's corner, 1031,57
    // -- not at 10,40, which is a slice of the desktop's top-left.
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {5});
    backend.set_root(1);
    backend.set_parent(5, 0x2600608);
    backend.set_geometry(5, Geometry{10, 40, 800, 626});
    backend.set_root_position(5, Point{1031, 89});
    backend.set_numbers(5, "_NET_FRAME_EXTENTS", {0, 0, 32, 0});
    Config config;
    WindowManager manager(config, backend);
    const auto info = manager.list_windows().front();
    CHECK_EQ(info.x, 1031);
    CHECK_EQ(info.y, 57);
    CHECK_EQ(info.width, 800);
    CHECK_EQ(info.height, 658);
    CHECK(info.placed);
    CHECK_EQ(info.frame, WindowId(0x2600608));
}

TEST_CASE(manager_a_direct_child_of_the_root_is_its_own_frame) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {5});
    backend.set_root(1);
    backend.set_parent(5, 1);  // reparented by the WM? no: its parent is the root
    Config config;
    WindowManager manager(config, backend);
    CHECK_EQ(manager.list_windows().front().frame, WindowId(5));
}

TEST_CASE(manager_an_unplaceable_window_reports_itself_as_unplaced) {
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {5});
    add_window(backend, 5, "brave", 10, "t", 0);
    backend.set_root_position(5, std::nullopt);
    Config config;
    WindowManager manager(config, backend);
    CHECK(!manager.list_windows().front().placed);
}

// --- talking to the window manager ----------------------------------------

TEST_CASE(manager_a_broken_connection_is_reported_not_raised) {
    testing::FakeBackend backend;
    Config config;
    WindowManager manager(config, backend);
    // The connection drops after startup, as it does in the wild: the requests
    // must be reported (false), not allowed to take the process down.
    backend.set_broken(true);
    CHECK(!manager.make_sticky(0x5A00003));
    CHECK(!manager.clear_attention(0x5A00003));
}

TEST_CASE(manager_an_empty_event_queue_is_not_an_error) {
    testing::FakeBackend backend;
    Config config;
    WindowManager manager(config, backend);
    int focus = 0;
    int desktop = 0;
    manager.on_focus_change = [&focus] { ++focus; };
    manager.on_desktop_change = [&desktop] { ++desktop; };
    manager.drain_events();
    CHECK_EQ(focus, 0);
    CHECK_EQ(desktop, 0);
}
