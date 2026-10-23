#include "capture.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <cerrno>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "log.hpp"

namespace wintab {
namespace detail {

// Grabbing through the X socket costs a round trip plus a copy of every pixel:
// measured on this machine, 25ms for a 1920x1080 root grab and ~23ms for each
// full-screen window. MIT-SHM hands the server a buffer to write into directly:
// 1ms for the same root grab, 7-12ms for a window pixmap. The segment is created
// once, grown when a window is larger than anything seen so far, and abandoned
// for good on the first failure so a server without MIT-SHM costs one attempt.
struct Shm {
    explicit Shm(Display* display) : display(display) {
        segment.shmid = -1;
        available = XShmQueryExtension(display) != 0;
    }

    ~Shm() { release(); }

    Shm(const Shm&) = delete;
    Shm& operator=(const Shm&) = delete;

    bool available = false;

    //: The drawable's top-left `width` x `height` corner, or an empty image when
    //: shared memory cannot be used (the caller then reads over the socket).
    Image grab(WindowId drawable, int width, int height, int depth) {
        if (!available || !drawable || width <= 1 || height <= 1) return Image();
        // A 32-bit (ARGB) window cannot be described by the screen's visual: leave
        // that one drawable to the socket path rather than spending the segment.
        if (depth != DefaultDepth(display, DefaultScreen(display))) return Image();
        if (!ensure(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)) {
            return Image();
        }
        XImage* image = XShmCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                                        static_cast<unsigned int>(depth), ZPixmap, segment.shmaddr,
                                        &segment, static_cast<unsigned int>(width),
                                        static_cast<unsigned int>(height));
        if (!image) return Image();
        const bool read = XShmGetImage(display, drawable, image, 0, 0, AllPlanes) != 0;
        Image pixels;
        if (read) {
            pixels = Image::from_ximage(
                image->data, static_cast<std::size_t>(image->bytes_per_line) * image->height,
                image->width, image->height, image->bytes_per_line);
        }
        // These pixels live in the segment; XDestroyImage must not free them.
        image->data = nullptr;
        XDestroyImage(image);
        // A refused request (no MIT-SHM server side, a window that went away)
        // would cost the same failed round trip on every later grab.
        if (!read) available = false;
        return pixels;
    }

private:
    bool ensure(std::size_t bytes) {
        if (attached && size >= bytes) return true;
        release();
        segment.shmid = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (segment.shmid < 0) {
            available = false;
            return false;
        }
        segment.shmaddr = static_cast<char*>(::shmat(segment.shmid, nullptr, 0));
        if (segment.shmaddr == reinterpret_cast<char*>(-1)) {
            segment.shmaddr = nullptr;
            release();
            available = false;
            return false;
        }
        segment.readOnly = False;
        XSync(display, False);
        if (!XShmAttach(display, &segment)) {
            release();
            available = false;
            return false;
        }
        XSync(display, False);
        attached = true;
        size = bytes;
        // Mark it for destruction now: the kernel frees the memory once both we
        // and the server detach, so even a SIGKILL cannot leave 8MB behind. The
        // id stays in the segment info because XShmDetach sends it back.
        ::shmctl(segment.shmid, IPC_RMID, nullptr);
        marked = true;
        return true;
    }

    void release() {
        if (segment.shmaddr) {
            if (attached) {
                XShmDetach(display, &segment);
                XSync(display, False);
                attached = false;
            }
            ::shmdt(segment.shmaddr);
            segment.shmaddr = nullptr;
        }
        if (segment.shmid >= 0) {
            if (!marked) ::shmctl(segment.shmid, IPC_RMID, nullptr);
            segment.shmid = -1;
            marked = false;
        }
        size = 0;
    }

    Display* display = nullptr;
    XShmSegmentInfo segment{};
    std::size_t size = 0;
    bool attached = false;
    bool marked = false;
};

}  // namespace detail

namespace {

double monotonic_now() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

//: Longest the worker waits for the screen to hold still before grabbing anyway.
constexpr double kMaxSettleWait = 1.0;
//: After a workspace switch the wait costs a swipe's worth of stale cards.
constexpr double kUrgentSettleWait = 0.25;

int read_current_desktop(Display* display, Window root) {
    Atom atom = XInternAtom(display, "_NET_CURRENT_DESKTOP", False);
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long items = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, root, atom, 0, 1, False, AnyPropertyType, &actual_type,
                           &actual_format, &items, &remaining, &data) != Success ||
        !data || items == 0) {
        if (data) XFree(data);
        return -1;
    }
    const int desktop = static_cast<int>(*reinterpret_cast<unsigned long*>(data));
    XFree(data);
    return desktop;
}

//: The slow path: the server copies every byte down the socket.
Image grab_over_socket(Display* display, Pixmap pixmap, unsigned int width,
                       unsigned int height) {
    XImage* image = XGetImage(display, pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!image) return Image();
    Image result = Image::from_ximage(image->data, static_cast<std::size_t>(image->bytes_per_line) *
                                                       static_cast<std::size_t>(image->height),
                                      image->width, image->height, image->bytes_per_line);
    XDestroyImage(image);
    return result;
}

Image grab_window(Display* display, detail::Shm* shm, WindowId frame) {
    if (!frame) return Image();
    int x = 0;
    int y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    Window root = 0;
    if (!XGetGeometry(display, frame, &root, &x, &y, &width, &height, &border, &depth)) {
        return Image();
    }
    if (width <= 1 || height <= 1) return Image();

    // The compositor's off-screen pixmap is the window's own pixels, even when
    // something is in front of it.
    Pixmap pixmap = XCompositeNameWindowPixmap(display, frame);
    if (!pixmap) return Image();
    Image result;
    if (shm) {
        result = shm->grab(pixmap, static_cast<int>(width), static_cast<int>(height),
                           static_cast<int>(depth));
    }
    if (!result) result = grab_over_socket(display, pixmap, width, height);
    XFreePixmap(display, pixmap);
    XFlush(display);
    return result;
}

}  // namespace

int stamped_desktop(int observed, int requested) { return observed >= 0 ? observed : requested; }

CapturePlan plan_capture(const std::vector<WinInfo>& everything, int desktop, const Rect& screen,
                         std::optional<int> our_pid) {
    CapturePlan plan;
    plan.desktop = desktop;
    const std::vector<WinInfo> windows = usable_windows(everything, desktop, our_pid);
    const std::set<WindowId> covered =
        windows_needing_own_pixels(everything, desktop, screen, our_pid);
    for (const WinInfo& win : windows) {
        // A minimized window has no on-screen pixels anywhere: its card is an
        // empty frame, not a crop of whatever sits where it used to be.
        if (!win.placed || win.minimized) continue;
        plan.targets.push_back(WindowTarget{win.xid, win.frame ? win.frame : win.xid, win.rect()});
        if (covered.count(win.xid)) continue;
        plan.crops[win.xid] = win.rect();
    }
    return plan;
}

ScreenCache::ScreenCache(const Config& config, UpdateFn on_update)
    : config_(config), on_update_(std::move(on_update)) {
    int fds[2] = {-1, -1};
    if (pipe(fds) == 0) {
        wake_read_ = fds[0];
        wake_write_ = fds[1];
        for (int fd : {wake_read_, wake_write_}) {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

ScreenCache::~ScreenCache() { stop(); }

void ScreenCache::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void ScreenCache::stop() {
    stop_.store(true);
    refresh_soon();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
    for (int* fd : {&wake_read_, &wake_write_}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
}

void ScreenCache::wake() {
    if (wake_write_ < 0) return;
    const char byte = 1;
    ssize_t ignored = ::write(wake_write_, &byte, 1);
    (void)ignored;
}

void ScreenCache::set_busy(bool busy) { busy_.store(busy); }

void ScreenCache::refresh_soon(bool urgent) {
    if (urgent) urgent_.store(true);
    dirty_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_ += 1;
    }
    wake();
}

void ScreenCache::set_targets(const std::vector<WindowTarget>& targets, int desktop) {
    std::lock_guard<std::mutex> lock(mutex_);
    targets_ = targets;
    requested_desktop_ = desktop;
}

std::optional<Snapshot> ScreenCache::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!plain_ || !backdrop_) return std::nullopt;
    Snapshot snapshot;
    snapshot.plain = plain_;
    snapshot.backdrop = backdrop_;
    snapshot.windows = windows_;
    snapshot.desktop = captured_desktop_;
    return snapshot;
}

int ScreenCache::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

std::string ScreenCache::stats_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const double age_value = captured_at_ == 0.0 ? -1.0 : monotonic_now() - captured_at_;
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "{'capture_ms': %.1f, 'age_s': %.1f, 'generation': %d, 'geometry': (%d, %d, %d, "
                  "%d), 'error': %s, 'xdamage': %s, 'windows': %d, 'desktop': %d, 'requests': %d}",
                  capture_ms_, age_value, generation_, 0, 0, capture_width_, capture_height_,
                  last_error_.empty() ? "None" : ("'" + last_error_ + "'").c_str(),
                  damage_ok_ ? "True" : "False", static_cast<int>(windows_.size()),
                  captured_desktop_, requests_);
    return buffer;
}

// --- worker ---------------------------------------------------------------

void ScreenCache::run() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = "cannot open display";
        return;
    }
    display_ = display;
    root_ = DefaultRootWindow(display);
    shm_ = std::make_unique<detail::Shm>(display);

    int event = 0;
    int error = 0;
    damage_ok_ = XDamageQueryExtension(display, &event, &error) != 0;
    damage_event_base_ = event;

    arm();
    capture();  // so the very first swipe already has pixels

    while (!stop_.load()) {
        await_change();
        if (stop_.load()) break;
        settle_and_capture();
    }

    disarm();
    shm_.reset();  // detach before the display goes away
    XCloseDisplay(display);
    display_ = nullptr;
}

void ScreenCache::arm() {
    Display* display = static_cast<Display*>(display_);
    if (!display || !damage_ok_ || damage_id_) return;
    damage_id_ = XDamageCreate(display, root_, XDamageReportNonEmpty);
    XFlush(display);
}

void ScreenCache::disarm() {
    Display* display = static_cast<Display*>(display_);
    if (!display || !damage_id_) return;
    XDamageDestroy(display, damage_id_);
    damage_id_ = 0;
    XFlush(display);
}

void ScreenCache::note_damage() {
    last_damage_at_ = monotonic_now();
    dirty_.store(true);
}

void ScreenCache::await_change() {
    Display* display = static_cast<Display*>(display_);
    if (!display) {
        for (int i = 0; i < 20 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return;
    }
    if (!damage_ok_) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::duration<double>(config_.capture_poll_s);
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stop_.load()) note_damage();
        return;
    }

    pollfd fds[2] = {{ConnectionNumber(display), POLLIN, 0}, {wake_read_, POLLIN, 0}};
    int ready = 0;
    do {
        ready = ::poll(fds, 2, 500);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return;

    if (fds[1].revents & POLLIN) {
        char sink[64];
        while (::read(wake_read_, sink, sizeof(sink)) > 0) {
        }
    }
    if (!(fds[0].revents & POLLIN)) return;

    while (XPending(display) > 0) {
        XEvent event{};
        XNextEvent(display, &event);
        if (event.type == damage_event_base_ + XDamageNotify) {
            note_damage();
            disarm();
        }
    }
}

void ScreenCache::settle_and_capture() {
    if (!dirty_.load()) return;

    const double first_pending = monotonic_now();
    while (!stop_.load()) {
        if (busy_.load()) {
            // A gesture is on screen: never grab pixels right now.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const double now = monotonic_now();
        const bool settled =
            last_damage_at_ > 0.0 && now - last_damage_at_ >= config_.capture_debounce_s;
        const double limit = urgent_.load() ? kUrgentSettleWait : kMaxSettleWait;
        const bool waited_too_long = now - first_pending >= limit;
        if (settled || waited_too_long) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    dirty_.store(false);
    urgent_.store(false);
    if (busy_.load()) {
        dirty_.store(true);
        return;
    }
    capture();
}

bool ScreenCache::capture() {
    Display* display = static_cast<Display*>(display_);
    if (!display || !root_) return false;

    int screen_x = 0;
    int screen_y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    Window root = 0;
    if (!XGetGeometry(display, root_, &root, &screen_x, &screen_y, &width, &height, &border,
                      &depth)) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = "could not read the root window";
        return false;
    }

    const double started = monotonic_now();
    int requested = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requested = requested_desktop_;
    }
    // Read the workspace *before* the pixels: the stamp has to describe the
    // moment the grab started.
    const int observed = read_current_desktop(display, root_);

    Image plain;
    if (shm_) {
        plain = shm_->grab(root_, static_cast<int>(width), static_cast<int>(height),
                           static_cast<int>(depth));
    }
    if (!plain) {
        XImage* image = XGetImage(display, root_, 0, 0, width, height, AllPlanes, ZPixmap);
        if (!image) {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = "could not read the root window";
            return false;
        }
        plain = Image::from_ximage(
            image->data, static_cast<std::size_t>(image->bytes_per_line) * image->height,
            image->width, image->height, image->bytes_per_line);
        XDestroyImage(image);
    }
    if (!plain) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = "could not decode the root window";
        return false;
    }

    std::map<WindowId, Image> windows = read_windows();
    Image backdrop = make_backdrop(plain);
    const double elapsed = (monotonic_now() - started) * 1000.0;

    int generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plain_ = plain;
        backdrop_ = backdrop;
        windows_ = std::move(windows);
        captured_desktop_ = stamped_desktop(observed, requested);
        capture_width_ = static_cast<int>(width);
        capture_height_ = static_cast<int>(height);
        captured_at_ = monotonic_now();
        capture_ms_ = elapsed;
        // Monotonic, like the Python build's ``self._generation += 1``: callers
        // wait for it to *change*, so a counter that restarts at 1 would never
        // report a second capture.
        generation = ++generation_;
        last_error_.clear();
    }

    arm();  // re-arm only once we have taken what we came for
    if (on_update_) {
        try {
            on_update_(generation);
        } catch (...) {
            // the capture thread must not die on a callback
        }
    }
    return true;
}

std::map<WindowId, Image> ScreenCache::read_windows() {
    Display* display = static_cast<Display*>(display_);
    if (!display) return {};

    std::vector<WindowTarget> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = targets_;
    }

    std::map<WindowId, Image> out;
    for (const WindowTarget& target : targets) {
        if (busy_.load()) {
            // A gesture came up: the rest can wait for the next capture.
            break;
        }
        Image picture = grab_window(display, shm_.get(), target.frame);
        if (picture) out[target.xid] = picture;
    }

    if (out.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!windows_.empty()) {
            // Nothing could be read this time -- the compositor is between
            // redirects, say. Previous pixels beat an empty strip.
            return windows_;
        }
    }
    return out;
}

Image ScreenCache::make_backdrop(const Image& plain) {
    const int scale = std::max(2, config_.blur_downscale);
    const int width = plain.width();
    const int height = plain.height();
    if (width <= 0 || height <= 0) return Image();

    // All the per-pixel work happens at 1/scale, then one upscale.
    Image small = resize_bilinear(plain, std::max(1, width / scale), std::max(1, height / scale));
    if (!small) return Image();
    small = gaussian_blur(small, config_.blur_radius);
    if (!small) return Image();
    if (config_.dim > 0.0) {
        small = blend_to_black(small, std::min(1.0, std::max(0.0, config_.dim)));
        if (!small) return Image();
    }
    // A little less colour makes the backdrop read as a background.
    small = desaturate(small, 0.9);
    if (!small) return Image();
    return resize_bicubic(small, width, height);
}

}  // namespace wintab
