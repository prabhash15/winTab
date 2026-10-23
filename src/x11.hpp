// The X11 seam.
//
// window.cpp's rules (filtering, ordering, placement) are pure logic; the X
// conversation behind them goes through this interface so the rules stay
// testable with an in-memory backend, exactly as the Python build fakes them.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace wintab {

using WindowId = unsigned long;
using AtomId = unsigned long;

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Point {
    int x = 0;
    int y = 0;
};

struct WindowAttributes {
    bool override_redirect = false;
};

struct Geometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

//: A property as X hands it over: 32-bit values for atoms/cardinals/windows,
//: 8-bit bytes for strings (WM_CLASS, _NET_WM_NAME).
struct PropertyValue {
    AtomId type = 0;
    int format = 0;
    std::vector<unsigned long> numbers;
    std::string bytes;

    bool present() const { return format != 0; }
    bool empty() const { return !present() || (numbers.empty() && bytes.empty()); }
};

struct PropertyEvent {
    AtomId atom = 0;
    double time = 0.0;
};

class XBackend {
public:
    virtual ~XBackend() = default;

    virtual PropertyValue property(WindowId window, AtomId atom) = 0;
    virtual std::string atom_name(AtomId atom) = 0;
    virtual AtomId intern(const std::string& name) = 0;

    virtual std::optional<WindowAttributes> attributes(WindowId window) = 0;
    virtual std::optional<Geometry> geometry(WindowId window) = 0;
    //: Where a window's top-left corner sits on the root window.
    virtual std::optional<Point> root_position(WindowId window) = 0;
    virtual WindowId parent_of(WindowId window) = 0;

    virtual void change_cardinal(WindowId window, AtomId atom, unsigned long value) = 0;
    virtual void send_state_message(WindowId window, int action, AtomId first, AtomId second) = 0;
    virtual void send_activate(WindowId window) = 0;

    virtual void watch_property_events() = 0;
    virtual WindowId root_window() = 0;

    virtual bool pending_events() = 0;
    //: The next PropertyNotify, skipping other events. False when the queue is
    //: empty.
    virtual bool next_property_event(PropertyEvent& out) = 0;
    virtual int fileno() = 0;
    virtual Rect screen_geometry() = 0;
};

//: Install the process-wide Xlib error handler once, chaining to GDK's so GDK's
//: error traps keep working. Call after gtk_init.
void install_x_error_handler();

//: The real thing: one Display, owned by the calling thread.
class XlibBackend final : public XBackend {
public:
    XlibBackend();
    ~XlibBackend() override;

    XlibBackend(const XlibBackend&) = delete;
    XlibBackend& operator=(const XlibBackend&) = delete;

    PropertyValue property(WindowId window, AtomId atom) override;
    std::string atom_name(AtomId atom) override;
    AtomId intern(const std::string& name) override;

    std::optional<WindowAttributes> attributes(WindowId window) override;
    std::optional<Geometry> geometry(WindowId window) override;
    std::optional<Point> root_position(WindowId window) override;
    WindowId parent_of(WindowId window) override;

    void change_cardinal(WindowId window, AtomId atom, unsigned long value) override;
    void send_state_message(WindowId window, int action, AtomId first, AtomId second) override;
    void send_activate(WindowId window) override;

    void watch_property_events() override;
    WindowId root_window() override;

    bool pending_events() override;
    bool next_property_event(PropertyEvent& out) override;
    int fileno() override;
    Rect screen_geometry() override;

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    Impl* impl_ = nullptr;
};

}  // namespace wintab
