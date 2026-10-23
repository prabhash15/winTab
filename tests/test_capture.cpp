#include <map>
#include <optional>
#include <string>
#include <vector>

#include "capture.hpp"
#include "config.hpp"
#include "support/fake_x11.hpp"
#include "support/harness.hpp"
#include "windows.hpp"

using namespace wintab;

namespace {

//: The screen every one of these tests draws on.
constexpr Rect kScreen{0, 0, 1920, 1080};

WinInfo win(WindowId xid, int x = 0, int y = 0, int width = 300, int height = 300,
             int desktop = 0) {
    WinInfo info;
    info.xid = xid;
    info.wm_class = "brave-browser";
    info.x = x;
    info.y = y;
    info.width = width;
    info.height = height;
    info.desktop = desktop;
    return info;
}

std::vector<WindowId> target_ids(const CapturePlan& plan) {
    std::vector<WindowId> out;
    for (const WindowTarget& target : plan.targets) out.push_back(target.xid);
    return out;
}

std::vector<WindowId> crop_ids(const CapturePlan& plan) {
    std::vector<WindowId> out;
    for (const auto& [xid, rect] : plan.crops) out.push_back(xid);
    return out;
}

}  // namespace

TEST_CASE(capture_the_servers_answer_stamps_the_grab) {
    CHECK_EQ(stamped_desktop(2, 1), 2);
    CHECK_EQ(stamped_desktop(0, 5), 0);
}

TEST_CASE(capture_an_unreadable_desktop_falls_back_to_the_request) {
    CHECK_EQ(stamped_desktop(-1, 3), 3);
}

// --- the plan: what is read, and where a crop is safe ---------------------

TEST_CASE(capture_every_placed_window_is_asked_for_its_own_pixels) {
    // A cascade plus one on its own: the covered ones used to be the only
    // targets, and everything else was cropped out of a screen grab.
    const std::vector<WinInfo> windows{win(1, 0, 0), win(2, 50, 50), win(3, 900, 0)};
    const auto plan = plan_capture(windows, 0, kScreen);
    CHECK_EQ(target_ids(plan), (std::vector<WindowId>{1, 2, 3}));
    CHECK_EQ(plan.desktop, 0);
}

TEST_CASE(capture_a_crop_is_only_offered_where_nothing_covers_the_window) {
    const std::vector<WinInfo> windows{win(1, 0, 0), win(2, 50, 50)};
    const auto plan = plan_capture(windows, 0, kScreen);
    // 1 is behind 2, so a crop of the screen there would show window 2.
    CHECK_EQ(crop_ids(plan), (std::vector<WindowId>{2}));
}

TEST_CASE(capture_a_minimized_window_is_neither_read_nor_cropped) {
    auto back = win(1, 0, 0);
    auto gone = win(2, 0, 0);
    gone.minimized = true;
    const auto plan = plan_capture({back, gone}, 0, kScreen);
    CHECK_EQ(target_ids(plan), (std::vector<WindowId>{1}));
    // Window 1 has nothing in front of it, so a crop is a fine fallback for it.
    CHECK_EQ(crop_ids(plan), (std::vector<WindowId>{1}));
}

TEST_CASE(capture_an_unplaceable_window_is_not_published) {
    auto real = win(1, 0, 0);
    auto ghost = win(2, 900, 0);
    ghost.placed = false;
    const auto plan = plan_capture({real, ghost}, 0, kScreen);
    CHECK_EQ(target_ids(plan), (std::vector<WindowId>{1}));
}

TEST_CASE(capture_only_this_workspace_is_published) {
    const std::vector<WinInfo> windows{win(1, 0, 0, 300, 300, 0), win(2, 0, 0, 300, 300, 1),
                                       win(3, 0, 0, 300, 300, -1)};
    const auto plan = plan_capture(windows, 0, kScreen);
    CHECK_EQ(target_ids(plan), (std::vector<WindowId>{1, 3}));
}

TEST_CASE(capture_the_frame_is_what_gets_read) {
    auto window = win(1, 0, 0);
    window.frame = 0xABC;
    const auto plan = plan_capture({window}, 0, kScreen);
    CHECK_EQ(plan.targets.front().frame, WindowId(0xABC));
}

TEST_CASE(capture_a_window_without_a_frame_falls_back_to_its_own_id) {
    auto window = win(7, 0, 0);
    window.frame = 0;
    const auto plan = plan_capture({window}, 0, kScreen);
    CHECK_EQ(plan.targets.front().frame, WindowId(7));
}

TEST_CASE(capture_the_workspace_is_stamped_on_the_targets) {
    const auto plan = plan_capture({win(1)}, 2, kScreen);
    CHECK_EQ(plan.desktop, 2);
}

// --- the window-model rules that feed the plan ----------------------------

TEST_CASE(capture_rects_overlap_only_when_they_share_area) {
    CHECK(rects_overlap(Rect{0, 0, 10, 10}, Rect{5, 5, 10, 10}));
    // Touching edges are not overlapping.
    CHECK(!rects_overlap(Rect{0, 0, 10, 10}, Rect{10, 0, 10, 10}));
    CHECK(!rects_overlap(Rect{0, 0, 10, 10}, Rect{0, 10, 10, 10}));
}

TEST_CASE(capture_nothing_overlaps_means_a_crop_is_enough) {
    const std::vector<WinInfo> windows{win(1, 0, 0, 400, 300), win(2, 500, 0, 400, 300)};
    const auto plan = plan_capture(windows, 0, kScreen);
    CHECK_EQ(crop_ids(plan).size(), std::size_t(2));
}

TEST_CASE(capture_the_front_window_still_gets_away_with_a_crop) {
    // Three windows in a cascade; only the frontmost one is uncropped by
    // anything, so the other two need reading from the compositor.
    const std::vector<WinInfo> windows{win(1, 0, 0), win(2, 50, 50), win(3, 100, 100)};
    const auto plan = plan_capture(windows, 0, kScreen);
    CHECK_EQ(crop_ids(plan), (std::vector<WindowId>{3}));
}

TEST_CASE(capture_a_window_hanging_off_the_screen_needs_its_own_pixels) {
    auto off = win(1, 1800, 0, 400, 300);
    const auto plan = plan_capture({off}, 0, kScreen);
    CHECK(crop_ids(plan).empty());
}

TEST_CASE(capture_windows_on_other_workspaces_do_not_cover_anything) {
    auto back = win(1, 0, 0, 400, 300);
    auto other = win(2, 0, 0, 400, 300, 3);
    const auto plan = plan_capture({back, other}, 0, kScreen);
    CHECK_EQ(crop_ids(plan), (std::vector<WindowId>{1}));
}

TEST_CASE(capture_a_panel_above_a_window_really_does_cover_it) {
    auto below = win(1, 0, 0, 400, 300);
    auto panel = win(2, 0, 0, 1920, 40);
    panel.dock = true;
    panel.wm_class = "cinnamon";
    const auto plan = plan_capture({below, panel}, 0, kScreen);
    CHECK(crop_ids(plan).empty());
}

TEST_CASE(capture_sticky_windows_are_compared_too) {
    auto sticky = win(1, 0, 0, 400, 300, -1);
    auto above = win(2, 0, 0, 400, 300, 0);
    // Both are on this workspace; window 2 sits on top of the sticky one, so
    // 1 is covered and 2 keeps its crop.
    const auto plan = plan_capture({sticky, above}, 0, kScreen);
    CHECK_EQ(crop_ids(plan), (std::vector<WindowId>{2}));
}

TEST_CASE(capture_our_own_overlay_never_covers_anything) {
    // The overlay is a full-screen window and is always on top: if the
    // stacking test counted it, every card would take the slow path.
    auto window = win(1, 0, 0, 400, 300);
    auto overlay = win(2, 0, 0, 1920, 1080);
    overlay.our_own = true;
    overlay.pid = 4242;
    const auto plan = plan_capture({window, overlay}, 0, kScreen, 4242);
    // Both are published, but the overlay is filtered out of the crop test by
    // its own pid, and the window behind nothing keeps its crop.
    CHECK_EQ(crop_ids(plan), (std::vector<WindowId>{1}));
}

// --- the manager that fills the plan from X --------------------------------

TEST_CASE(capture_an_unreadable_window_is_skipped_not_fatal) {
    // A window the server refuses to describe (no geometry, no class) is
    // dropped rather than crashing the enumeration; the rest still publish.
    testing::FakeBackend backend;
    backend.set_root_numbers("_NET_CLIENT_LIST_STACKING", {1, 9999});
    backend.set_root_numbers("_NET_CURRENT_DESKTOP", {0});
    backend.set_geometry(1, Geometry{0, 0, 800, 600});
    backend.set_root_position(1, Point{10, 20});
    backend.set_bytes(1, "WM_CLASS", std::string("i\0brave\0"));
    backend.set_numbers(1, "_NET_WM_PID", {1000});
    backend.set_bytes(1, "_NET_WM_NAME", std::string("Inbox"));
    backend.set_numbers(1, "_NET_WM_DESKTOP", {0});
    std::vector<unsigned long> types;
    types.push_back(backend.intern("_NET_WM_WINDOW_TYPE_NORMAL"));
    backend.set_numbers(1, "_NET_WM_WINDOW_TYPE", types);
    // An override-redirect window (a menu, a tooltip) is not a real app
    // window: describe() drops it, and the enumeration survives it.
    backend.set_override_redirect(9999, true);
    Config config;
    WindowManager manager(config, backend);
    const auto windows = manager.list_windows();
    CHECK_EQ(windows.size(), std::size_t(1));
    CHECK_EQ(windows.front().xid, WindowId(1));
}
