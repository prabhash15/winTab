// Raw multitouch contacts from the touchpad's evdev node.
//
// libinput only hands out already-parsed *gesture* events, which is exactly what
// a finger-following selection must not use, so this reads the kernel's
// multitouch protocol straight from /dev/input/event* -- per-slot contact
// positions at the pad's own report rate, no extra privileges beyond membership
// of the `input` group.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wintab {

//: Slots are 0..N; more than eight simultaneous fingers is not a three-finger
//: swipe, so a fixed array avoids an allocation on the ~140Hz input path.
inline constexpr int kMaxContacts = 8;

struct PadGeometry {
    int x_min = 0;
    int x_max = 1;
    int y_min = 0;
    int y_max = 1;

    //: Approximate, only used for human-readable messages.
    std::pair<double, double> normalise(int x, int y) const;
};

struct Contact {
    int slot = 0;
    double x = 0.0;
    double y = 0.0;
};

struct ContactFrame {
    double time = 0.0;
    std::array<Contact, kMaxContacts> contacts{};
    int count = 0;
    bool dropped = false;
    int btn_fingers = 0;

    std::optional<std::pair<double, double>> centroid() const;
};

ContactFrame make_frame(double time, std::initializer_list<Contact> contacts,
                        bool dropped = false, int btn_fingers = 0);

//: The touchpad node and name found in /proc/bus/input/devices.
struct TouchpadDevice {
    std::string node;
    std::string name;
};

//: Parse /proc/bus/input/devices into blocks of key -> value ("N" is the name,
//: "H" the handlers).
std::vector<std::map<std::string, std::string>> parse_input_devices(const std::string& text);

//: Find the pad. `name` is a case-insensitive substring; without one the first
//: device whose name contains "touchpad" wins. Throws std::runtime_error with
//: the message the Python build prints.
TouchpadDevice find_touchpad(const std::optional<std::string>& name = std::nullopt);

//: The touchpad node disappeared (suspend, unplug, driver reload).
class DeviceLost : public std::runtime_error {
public:
    explicit DeviceLost(const std::string& what) : std::runtime_error(what) {}
};

//: Blocking-ish source reading an evdev touchpad node.
class EvdevSource {
public:
    EvdevSource(std::string node, std::string name = "", bool exclusive = false);
    ~EvdevSource();

    EvdevSource(const EvdevSource&) = delete;
    EvdevSource& operator=(const EvdevSource&) = delete;

    const PadGeometry& geometry() const { return geometry_; }
    const std::string& node() const { return node_; }
    const std::string& name() const { return name_; }
    bool closed() const { return closed_; }
    int fileno() const { return fd_; }
    void close();

    //: Wait for input, then return every frame that arrived.
    std::vector<ContactFrame> read_frames(double timeout = 0.5);

private:
    std::vector<ContactFrame> decode(const unsigned char* data, std::size_t length);
    void apply_abs(int code, int value);
    ContactFrame build_frame(double now) const;

    struct Slot {
        bool active = false;
        int x = 0;
        int y = 0;
    };

    std::string node_;
    std::string name_;
    int fd_ = -1;
    bool closed_ = false;
    PadGeometry geometry_;
    std::array<Slot, 16> slots_{};
    int current_slot_ = 0;
    int btn_fingers_ = 0;
    std::vector<unsigned char> buffer_;
};

//: Runs an EvdevSource in a thread and hands frames to a callback. The callback
//: runs on the reader thread.
class TouchpadReader {
public:
    using FrameHandler = std::function<void(const std::vector<ContactFrame>&)>;
    using DeviceHandler = std::function<void(const std::string&, const PadGeometry*)>;

    TouchpadReader(FrameHandler on_frames, std::optional<std::string> name = std::nullopt,
                   DeviceHandler on_device_change = nullptr, double reconnect_delay = 2.0);
    ~TouchpadReader();

    TouchpadReader(const TouchpadReader&) = delete;
    TouchpadReader& operator=(const TouchpadReader&) = delete;

    void start();
    void stop();
    bool running() const;

private:
    void run();
    void report(const std::string& message, const PadGeometry* geometry);

    FrameHandler on_frames_;
    std::optional<std::string> name_;
    DeviceHandler on_device_change_;
    double reconnect_delay_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

// --- recording and replay (the JSONL contract) ----------------------------

//: One frame <-> one JSONL line: {"t": 12.345678, "c": [[0, 0.1, 0.2]], "dropped": false}
std::string format_frames(const std::vector<ContactFrame>& frames);
bool parse_frame_record(const std::string& line, ContactFrame& out);

//: Replay lines in real time, sleeping between records (capped at max_sleep).
void replay_stream(std::istream& in, double max_sleep,
                   const std::function<void(const ContactFrame&)>& on_frame);

}  // namespace wintab
