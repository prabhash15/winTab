// Where each window really is, and does switching work.
//
// Prints every managed window's place on screen next to the rect the app
// computed, which cards would need their own pixels (because a crop would show
// something else), and -- with --activate -- walks the workspace asking the
// window manager to activate each window with the app's own request.
//
//     probe_windows
//     probe_windows --activate
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "config.hpp"
#include "log.hpp"
#include "windows.hpp"
#include "x11.hpp"

namespace {

std::string describe_flags(const wintab::WinInfo& win, bool covered) {
    std::string flags;
    if (win.minimized) flags += " minimized";
    if (!win.placed) flags += " unplaced";
    if (covered) flags += " needs-own-pixels";
    if (win.dock) flags += " dock";
    if (win.desktop_window) flags += " desktop";
    return flags;
}

}  // namespace

int main(int argc, char** argv) {
    bool activate = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--activate") == 0) activate = true;
    }

    wintab::Config config = wintab::load_config();
    try {
        wintab::XlibBackend backend;
        wintab::WindowManager manager(config, backend);
        const auto windows = manager.list_windows(true);
        const int desktop = manager.current_desktop();
        const auto screen = manager.screen_geometry();
        const auto covered = wintab::windows_needing_own_pixels(windows, desktop, screen);
        const auto usable = wintab::usable_windows(windows, desktop, manager.own_pid());

        std::printf("screen %dx%d, desktop %d of %d, %zu managed window(s)\n", screen.w, screen.h,
                    desktop, manager.desktop_count(), windows.size());
        for (const auto& win : windows) {
            std::printf("0x%-8lx %-24s %5d,%-5d %4dx%-4d desktop=%-3d frame=0x%-8lx%s\n", win.xid,
                        win.wm_class.c_str(), win.x, win.y, win.width, win.height, win.desktop,
                        win.frame, describe_flags(win, covered.count(win.xid) > 0).c_str());
            if (!win.title.empty()) std::printf("           title: %s\n", win.title.c_str());
        }

        std::printf("\n%d window(s) in the strip on this workspace:\n", static_cast<int>(usable.size()));
        for (const auto& win : usable) {
            std::printf("  0x%-8lx %s\n", win.xid,
                        win.wm_class.empty() ? win.title.c_str() : win.wm_class.c_str());
        }

        if (!activate) return 0;

        std::printf("\nasking the window manager to activate each window...\n");
        for (const auto& win : usable) {
            manager.activate(win.xid);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            const auto active = manager.active_window();
            const bool took = active && *active == win.xid;
            std::printf("  0x%-8lx -> %s (active is 0x%lx)\n", win.xid,
                        took ? "focus followed" : "FOCUS DID NOT FOLLOW",
                        active ? *active : 0UL);
        }
        return 0;
    } catch (const std::exception& exc) {
        wintab::log_line(std::string("probe_windows: ") + exc.what());
        return 1;
    }
}
