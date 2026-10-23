// Prove the overlay window recipe under this compositor, without the app.
//
// The strip's visibility depends on a very specific window: undecorated,
// NOTIFICATION type, skip taskbar/pager, not focusable, RGBA visual,
// app-paintable, and an empty input shape so every click falls through. This
// builds exactly that window, paints it magenta with a white marker, grabs the
// screen, and checks:
//
//   * the window is managed and mapped, and sits on top of the stack
//   * it did not steal focus
//   * its pixels actually reached the screen (the white marker is visible)
//   * it covers the top edge, where the strip's band lives
//
//     ./build/spike_overlay            # run the checks
//     ./build/spike_overlay --keep     # leave it on screen until Enter
//     ./build/spike_overlay --save /tmp/overlay.png
#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "image.hpp"
#include "x11.hpp"

using namespace wintab;

namespace {

struct Pixels {
    int width = 0;
    int height = 0;
};

struct Report {
    unsigned long active = 0;
    std::vector<unsigned long> stacking;
    Image screenshot;
};

Pixels screen_size(Display* display) {
    return Pixels{XDisplayWidth(display, DefaultScreen(display)),
                  XDisplayHeight(display, DefaultScreen(display))};
}

double now_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void pump(double seconds) {
    GMainContext* context = g_main_context_default();
    const double deadline = now_seconds() + seconds;
    while (now_seconds() < deadline) {
        while (g_main_context_pending(context)) g_main_context_iteration(context, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

unsigned long property_number(Display* display, unsigned long window, const char* name) {
    Atom atom = XInternAtom(display, name, False);
    Atom actual = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, window, atom, 0, 1, False, AnyPropertyType,
                                          &actual, &format, &count, &after, &data);
    unsigned long value = 0;
    if (status == Success && data && count) value = reinterpret_cast<unsigned long*>(data)[0];
    if (data) XFree(data);
    return value;
}

std::vector<unsigned long> property_windows(Display* display, unsigned long window,
                                            const char* name) {
    std::vector<unsigned long> values;
    Atom atom = XInternAtom(display, name, False);
    Atom actual = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(display, window, atom, 0, 4096, False, AnyPropertyType,
                                          &actual, &format, &count, &after, &data);
    if (status == Success && data && format == 32) {
        const auto* numbers = reinterpret_cast<unsigned long*>(data);
        for (unsigned long i = 0; i < count; ++i) values.push_back(numbers[i]);
    }
    if (data) XFree(data);
    return values;
}

std::string window_name(Display* display, unsigned long window) {
    for (const char* name : {"_NET_WM_NAME", "WM_CLASS", "WM_NAME"}) {
        Atom atom = XInternAtom(display, name, False);
        Atom actual = 0;
        int format = 0;
        unsigned long count = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        const int status = XGetWindowProperty(display, window, atom, 0, 256, False,
                                              AnyPropertyType, &actual, &format, &count, &after,
                                              &data);
        std::string text;
        if (status == Success && data && count) {
            if (format == 8) {
                text.assign(reinterpret_cast<const char*>(data), count);
                const std::size_t nul = text.find('\0');
                if (nul != std::string::npos) text = text.substr(0, nul);
            }
        }
        if (data) XFree(data);
        if (!text.empty()) return text;
    }
    return "?";
}

std::vector<std::string> atom_names(Display* display, unsigned long window, const char* name) {
    std::vector<std::string> names;
    for (unsigned long atom : property_windows(display, window, name)) {
        char* text = XGetAtomName(display, static_cast<Atom>(atom));
        if (text) {
            names.emplace_back(text);
            XFree(text);
        }
    }
    return names;
}

Report report(Display* display, unsigned long xid, const Pixels& size) {
    Report out;
    const unsigned long root = DefaultRootWindow(display);
    XSync(display, False);
    out.active = property_number(display, root, "_NET_ACTIVE_WINDOW");
    out.stacking = property_windows(display, root, "_NET_CLIENT_LIST_STACKING");
    (void)xid;
    XImage* image = XGetImage(display, root, 0, 0, size.width, size.height, AllPlanes, ZPixmap);
    if (image) {
        out.screenshot = Image::from_ximage(
            image->data, static_cast<std::size_t>(image->bytes_per_line) * image->height,
            image->width, image->height, image->bytes_per_line);
        XDestroyImage(image);
    }
    XFlush(display);
    return out;
}

const char* kinds(Display* display, unsigned long window, std::string& scratch) {
    scratch.clear();
    for (const std::string& name : atom_names(display, window, "_NET_WM_WINDOW_TYPE")) {
        if (!scratch.empty()) scratch += ",";
        std::string short_name = name;
        const std::string prefix = "_NET_WM_WINDOW_TYPE_";
        if (short_name.rfind(prefix, 0) == 0) short_name = short_name.substr(prefix.size());
        scratch += short_name;
    }
    return scratch.c_str();
}

bool all_above(std::uint32_t pixel, int level) {
    const int r = (pixel >> 16) & 0xff;
    const int g = (pixel >> 8) & 0xff;
    const int b = pixel & 0xff;
    return r > level && g > level && b > level;
}

}  // namespace

int main(int argc, char** argv) {
    bool keep = false;
    double seconds = 2.5;
    std::optional<std::string> save;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--keep") == 0) {
            keep = true;
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (std::strncmp(argv[i], "--seconds=", 10) == 0) {
            seconds = std::atof(argv[i] + 10);
        } else if (std::strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            save = argv[++i];
        } else if (std::strncmp(argv[i], "--save=", 7) == 0) {
            save = std::string(argv[i] + 7);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf(
                "usage: spike_overlay [--keep] [--seconds N] [--save FILE.png]\n"
                "\n"
                "Builds the strip's exact overlay window, paints it, and checks that\n"
                "the compositor shows it on top without stealing focus.\n");
            return 0;
        }
    }

    gtk_init(nullptr, nullptr);
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::printf("spike: cannot open DISPLAY\n");
        return 1;
    }
    const Pixels size = screen_size(display);
    const unsigned long root = DefaultRootWindow(display);
    const unsigned long before = property_number(display, root, "_NET_ACTIVE_WINDOW");
    std::printf("screen                     : %dx%d\n", size.width, size.height);
    std::printf("focused window before      : 0x%lx\n", before);

    // Exactly how the real overlay is configured.
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    GdkScreen* screen = gdk_screen_get_default();
    GdkVisual* visual = screen ? gdk_screen_get_rgba_visual(screen) : nullptr;
    if (visual) gtk_widget_set_visual(window, visual);
    gtk_widget_set_app_paintable(window, TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), size.width, size.height);
    gtk_window_move(GTK_WINDOW(window), 0, 0);
    gtk_widget_show_all(window);

    GdkWindow* gdk_window = gtk_widget_get_window(window);
    if (!gdk_window) {
        std::printf("spike: the overlay window was not created; is DISPLAY set?\n");
        return 1;
    }
    // Empty input shape: every click falls through to what is underneath.
    cairo_region_t* empty = cairo_region_create();
    gdk_window_input_shape_combine_region(gdk_window, empty, 0, 0);
    cairo_region_destroy(empty);
    gdk_window_raise(gdk_window);

    const unsigned long xid = gdk_x11_window_get_xid(gdk_window);
    std::printf("overlay window id          : 0x%lx\n", xid);

    g_signal_connect(
        window, "draw",
        G_CALLBACK(+[](GtkWidget*, cairo_t* context, gpointer data) -> gboolean {
            const Pixels* size = static_cast<const Pixels*>(data);
            cairo_set_source_rgba(context, 1.0, 0.0, 1.0, 0.5);  // magenta, half transparency
            cairo_paint(context);
            cairo_set_source_rgb(context, 1.0, 1.0, 1.0);
            cairo_rectangle(context, size->width / 2 - 120, size->height / 2 - 60, 240, 120);
            cairo_fill(context);
            return FALSE;
        }),
        const_cast<Pixels*>(&size));
    gtk_widget_queue_draw(window);

    if (keep) std::printf("overlay is up; press Enter to finish\n");
    std::fflush(stdout);
    if (keep) {
        // Draw for real, then wait for the user, then look.
        pump(0.5);
        std::getchar();
    } else {
        pump(seconds);
    }

    const Report shot = report(display, xid, size);
    const std::vector<unsigned long>& stacking = shot.stacking;
    int own_index = -1;
    for (std::size_t i = 0; i < stacking.size(); ++i) {
        if (stacking[i] == xid) own_index = static_cast<int>(i);
    }

    std::printf("\n=== stacking (bottom -> top) ===\n");
    for (std::size_t position = 0; position < stacking.size(); ++position) {
        const unsigned long window_id = stacking[position];
        std::string scratch;
        std::string label = window_name(display, window_id);
        if (label.size() > 44) label = label.substr(0, 44);
        std::printf("  %2d  0x%-9lx %-44s [%s]%s\n", static_cast<int>(position), window_id,
                    label.c_str(), kinds(display, window_id, scratch),
                    window_id == xid ? " <-- overlay" : "");
    }

    struct Check {
        std::string label;
        bool ok;
    };
    std::vector<Check> checks;
    checks.push_back({"overlay is a managed, mapped window", own_index >= 0});
    checks.push_back({"overlay is on top (index " + std::to_string(own_index) + " of " +
                          std::to_string(static_cast<int>(stacking.size()) - 1) + ")",
                      !stacking.empty() && own_index == static_cast<int>(stacking.size()) - 1});
    char focus[160];
    std::snprintf(focus, sizeof(focus), "focus was not stolen (was 0x%lx, now 0x%lx)", before,
                  shot.active);
    checks.push_back({focus, shot.active == before});

    std::printf("\n=== checks ===\n");
    if (shot.screenshot) {
        const int centre_x = size.width / 2;
        const int centre_y = size.height / 2;
        const std::uint32_t centre = pixel_at(shot.screenshot, centre_x, centre_y);
        const std::uint32_t corner = pixel_at(shot.screenshot, centre_x, 8);
        const std::uint32_t panel = pixel_at(shot.screenshot, centre_x, size.height - 8);
        std::printf("  pixel centre (x%d,y%d)  = #%06x\n", centre_x, centre_y, centre);
        std::printf("  pixel top edge (titlebar/panel area) = #%06x\n", corner);
        std::printf("  pixel bottom edge (panel area)       = #%06x\n", panel);
        checks.push_back({"overlay pixels reached the screen (white marker visible)",
                          all_above(centre, 200)});
        checks.push_back({"overlay covers the top edge of the screen",
                          ((corner >> 16) & 0xff) > 100 && (corner & 0xff) > 100});
        if (save) {
            if (cairo_surface_write_to_png(shot.screenshot.surface(), save->c_str()) ==
                CAIRO_STATUS_SUCCESS) {
                std::printf("  saved: %s\n", save->c_str());
            }
        }
    }
    for (const Check& check : checks) {
        std::printf("  [%s] %s\n", check.ok ? "PASS" : "FAIL", check.label.c_str());
    }

    gtk_widget_destroy(window);
    XCloseDisplay(display);
    return std::all_of(checks.begin(), checks.end(),
                       [](const Check& check) { return check.ok; })
               ? 0
               : 1;
}
