#include "x11.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>

#include <map>
#include <stdexcept>

namespace wintab {
namespace {

XErrorHandler g_previous_handler = nullptr;
bool g_handler_installed = false;

Display* gdk_x_display() {
    GdkDisplay* display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) return nullptr;
    return GDK_DISPLAY_XDISPLAY(display);
}

int handle_x_error(Display* display, XErrorEvent* event) {
    // GDK's own error traps rely on its handler; hand those back untouched.
    if (g_previous_handler && display == gdk_x_display()) {
        return g_previous_handler(display, event);
    }
    // Everything else is a refusal we already cope with: the window manager
    // declining a state change, a window that went away mid-grab.
    return 0;
}

}  // namespace

void install_x_error_handler() {
    if (g_handler_installed) return;
    g_handler_installed = true;
    g_previous_handler = XSetErrorHandler(handle_x_error);
}

// --- XlibBackend ----------------------------------------------------------

struct XlibBackend::Impl {
    Display* display = nullptr;
    Window root = 0;
    std::map<std::string, AtomId> atoms;
};

XlibBackend::XlibBackend() : impl_(new Impl()) {
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) {
        throw std::runtime_error("cannot open display: is DISPLAY set?");
    }
    impl_->root = DefaultRootWindow(impl_->display);
}

XlibBackend::~XlibBackend() {
    if (impl_) {
        if (impl_->display) XCloseDisplay(impl_->display);
        delete impl_;
        impl_ = nullptr;
    }
}

AtomId XlibBackend::intern(const std::string& name) {
    auto it = impl_->atoms.find(name);
    if (it != impl_->atoms.end()) return it->second;
    const AtomId atom = XInternAtom(impl_->display, name.c_str(), False);
    impl_->atoms.emplace(name, atom);
    return atom;
}

std::string XlibBackend::atom_name(AtomId atom) {
    char* text = XGetAtomName(impl_->display, atom);
    if (!text) return {};
    std::string name(text);
    XFree(text);
    return name;
}

PropertyValue XlibBackend::property(WindowId window, AtomId atom) {
    PropertyValue out;
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(impl_->display, window, atom, 0, 0x1FFFFFFF, False,
                                         AnyPropertyType, &actual_type, &actual_format, &items,
                                         &remaining, &data);
    if (status != Success || !data) return out;
    out.type = actual_type;
    out.format = actual_format;
    if (actual_format == 32) {
        const auto* values = reinterpret_cast<unsigned long*>(data);
        out.numbers.assign(values, values + items);
    } else if (actual_format == 8) {
        out.bytes.assign(reinterpret_cast<char*>(data), items);
    }
    XFree(data);
    if (items == 0) out.format = 0;  // empty is "absent", like _prop
    return out;
}

std::optional<WindowAttributes> XlibBackend::attributes(WindowId window) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(impl_->display, window, &attributes)) return std::nullopt;
    return WindowAttributes{attributes.override_redirect != 0};
}

std::optional<Geometry> XlibBackend::geometry(WindowId window) {
    Window root = 0;
    int x = 0;
    int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    if (!XGetGeometry(impl_->display, window, &root, &x, &y, &width, &height, &border, &depth)) {
        return std::nullopt;
    }
    return Geometry{x, y, static_cast<int>(width), static_cast<int>(height)};
}

std::optional<Point> XlibBackend::root_position(WindowId window) {
    int x = 0;
    int y = 0;
    Window child = 0;
    if (!XTranslateCoordinates(impl_->display, window, impl_->root, 0, 0, &x, &y, &child)) {
        return std::nullopt;
    }
    return Point{x, y};
}

WindowId XlibBackend::parent_of(WindowId window) {
    Window root = 0;
    Window parent = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(impl_->display, window, &root, &parent, &children, &count)) return 0;
    if (children) XFree(children);
    return static_cast<WindowId>(parent);
}

void XlibBackend::change_cardinal(WindowId window, AtomId atom, unsigned long value) {
    XChangeProperty(impl_->display, window, atom, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&value), 1);
    XFlush(impl_->display);
}

void XlibBackend::send_state_message(WindowId window, int action, AtomId first, AtomId second) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.display = impl_->display;
    event.xclient.window = window;
    event.xclient.message_type = intern("_NET_WM_STATE");
    event.xclient.format = 32;
    event.xclient.data.l[0] = action;
    event.xclient.data.l[1] = static_cast<long>(first);
    event.xclient.data.l[2] = static_cast<long>(second);
    event.xclient.data.l[3] = 1;  // source indication: application
    event.xclient.data.l[4] = 0;
    XSendEvent(impl_->display, impl_->root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(impl_->display);
}

void XlibBackend::send_activate(WindowId window) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.display = impl_->display;
    event.xclient.window = window;
    event.xclient.message_type = intern("_NET_ACTIVE_WINDOW");
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;  // source indication: pager
    // The gesture is a user action happening now; raw evdev touches have no X
    // timestamp, and a remembered one is exactly the bug this replaces.
    event.xclient.data.l[1] = CurrentTime;
    event.xclient.data.l[2] = 0;
    event.xclient.data.l[3] = 0;
    event.xclient.data.l[4] = 0;
    XSendEvent(impl_->display, impl_->root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(impl_->display);
}

void XlibBackend::watch_property_events() {
    XSelectInput(impl_->display, impl_->root, PropertyChangeMask);
    XFlush(impl_->display);
}

WindowId XlibBackend::root_window() { return impl_->root; }

bool XlibBackend::pending_events() { return XPending(impl_->display) > 0; }

bool XlibBackend::next_property_event(PropertyEvent& out) {
    while (XPending(impl_->display) > 0) {
        XEvent event{};
        XNextEvent(impl_->display, &event);
        if (event.type != PropertyNotify) continue;
        out.atom = event.xproperty.atom;
        out.time = static_cast<double>(event.xproperty.time);
        return true;
    }
    return false;
}

int XlibBackend::fileno() { return ConnectionNumber(impl_->display); }

Rect XlibBackend::screen_geometry() {
    const int screen = DefaultScreen(impl_->display);
    return Rect{0, 0, DisplayWidth(impl_->display, screen), DisplayHeight(impl_->display, screen)};
}

}  // namespace wintab
