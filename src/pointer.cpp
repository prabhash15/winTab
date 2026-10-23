#include "pointer.hpp"

#include <X11/Xlib.h>

#include <cstdio>

namespace wintab {

PointerLock::~PointerLock() { release(); }

bool PointerLock::grab() {
    if (grabbed_) return true;
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        error_ = "cannot open display";
        return false;
    }
    Window root = DefaultRootWindow(display);
    Window root_return = 0;
    Window child_return = 0;
    int root_x = 0;
    int root_y = 0;
    int win_x = 0;
    int win_y = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(display, root, &root_return, &child_return, &root_x, &root_y, &win_x, &win_y,
                       &mask)) {
        error_ = "cannot query the pointer";
        XCloseDisplay(display);
        return false;
    }

    XSetWindowAttributes attributes{};
    attributes.override_redirect = True;
    attributes.event_mask = 0;
    Window confine = XCreateWindow(display, root, root_x, root_y, 1, 1, 0, CopyFromParent,
                                   InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask,
                                   &attributes);
    if (!confine) {
        error_ = "cannot create the confine window";
        XCloseDisplay(display);
        return false;
    }
    XMapWindow(display, confine);
    XFlush(display);

    // owner_events false, empty event mask: report to the grab window and
    // deliver nothing, so no unread queue can pile up.
    const int status = XGrabPointer(display, confine, False, 0, GrabModeAsync, GrabModeAsync,
                                    confine, None, CurrentTime);
    XFlush(display);
    if (status != GrabSuccess) {
        error_ = "grab refused (status " + std::to_string(status) + ")";
        XDestroyWindow(display, confine);
        XCloseDisplay(display);
        return false;
    }

    display_ = display;
    window_ = confine;
    grabbed_ = true;
    error_.clear();
    return true;
}

void PointerLock::release() {
    Display* display = static_cast<Display*>(display_);
    display_ = nullptr;
    const Window window = window_;
    window_ = 0;
    const bool was_grabbed = grabbed_;
    grabbed_ = false;
    if (!display) return;
    if (was_grabbed) XUngrabPointer(display, CurrentTime);
    if (window) XDestroyWindow(display, window);
    XFlush(display);
    XCloseDisplay(display);
}

}  // namespace wintab
