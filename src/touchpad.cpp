#include "touchpad.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <sstream>

#include "config.hpp"  // python_float, for the JSONL contract

namespace wintab {
namespace {

double monotonic_now() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto is_space = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string lower(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

//: The node in a HANDLERS line: "... event12 ..." -> /dev/input/event12.
//: Same match as the Python build's ``\bevent([0-9]+)\b``, without a regex
//: engine in the guest (std::regex costs ~50ms of construction and 200KB of code
//: to find one number).
std::optional<std::string> event_node(const std::string& handlers) {
    const auto is_word = [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_';
    };
    const std::string needle = "event";
    std::size_t at = handlers.find(needle);
    while (at != std::string::npos) {
        const bool starts_word = at == 0 || !is_word(static_cast<unsigned char>(handlers[at - 1]));
        const std::size_t first_digit = at + needle.size();
        std::size_t end = first_digit;
        while (end < handlers.size() && std::isdigit(static_cast<unsigned char>(handlers[end])) != 0) {
            ++end;
        }
        const bool ends_word =
            end == handlers.size() || !is_word(static_cast<unsigned char>(handlers[end]));
        if (starts_word && end > first_digit && ends_word) {
            return "/dev/input/event" + handlers.substr(first_digit, end - first_digit);
        }
        at = handlers.find(needle, at + 1);
    }
    return std::nullopt;
}

int fingers_for_button(int code) {
    switch (code) {
        case BTN_TOOL_FINGER: return 1;
        case BTN_TOOL_DOUBLETAP: return 2;
        case BTN_TOOL_TRIPLETAP: return 3;
        case BTN_TOOL_QUADTAP: return 4;
        case BTN_TOOL_QUINTTAP: return 5;
        default: return 0;
    }
}

bool is_finger_button(int code) {
    return code == BTN_TOOL_FINGER || code == BTN_TOOL_DOUBLETAP ||
           code == BTN_TOOL_TRIPLETAP || code == BTN_TOOL_QUADTAP ||
           code == BTN_TOOL_QUINTTAP;
}

PadGeometry read_geometry(int fd) {
    PadGeometry geometry;
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) {
        geometry.x_min = info.minimum;
        geometry.x_max = info.maximum;
    }
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) {
        geometry.y_min = info.minimum;
        geometry.y_max = info.maximum;
    }
    if (geometry.x_max <= geometry.x_min) {
        if (ioctl(fd, EVIOCGABS(ABS_X), &info) == 0) {
            geometry.x_min = info.minimum;
            geometry.x_max = info.maximum;
        }
    }
    if (geometry.y_max <= geometry.y_min) {
        if (ioctl(fd, EVIOCGABS(ABS_Y), &info) == 0) {
            geometry.y_min = info.minimum;
            geometry.y_max = info.maximum;
        }
    }
    if (geometry.x_max <= geometry.x_min) {
        geometry.x_min = 0;
        geometry.x_max = 1;
    }
    if (geometry.y_max <= geometry.y_min) {
        geometry.y_min = 0;
        geometry.y_max = 1;
    }
    return geometry;
}

// Minimal scanner for the one JSONL shape format_frames() writes. Kept small on
// purpose: the format is ours, and a full JSON library would be a dependency
// for three fields.
class JsonCursor {
public:
    explicit JsonCursor(const std::string& text) : text_(text) {}

    bool seek(const std::string& key) {
        const std::string needle = "\"" + key + "\"";
        std::size_t at = text_.find(needle, pos_);
        if (at == std::string::npos) return false;
        at = text_.find(':', at + needle.size());
        if (at == std::string::npos) return false;
        pos_ = at + 1;
        return true;
    }
    void skip_space() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    bool number(double& out) {
        skip_space();
        char* end = nullptr;
        const char* start = text_.c_str() + pos_;
        out = std::strtod(start, &end);
        if (end == start) return false;
        pos_ += static_cast<std::size_t>(end - start);
        return true;
    }
    bool integer(int& out) {
        skip_space();
        char* end = nullptr;
        const char* start = text_.c_str() + pos_;
        const long value = std::strtol(start, &end, 10);
        if (end == start) return false;
        pos_ += static_cast<std::size_t>(end - start);
        out = static_cast<int>(value);
        return true;
    }
    bool literal(const char* word) {
        skip_space();
        const std::size_t length = std::strlen(word);
        if (text_.compare(pos_, length, word) != 0) return false;
        pos_ += length;
        return true;
    }
    bool character(char wanted) {
        skip_space();
        if (pos_ >= text_.size() || text_[pos_] != wanted) return false;
        ++pos_;
        return true;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;
};

}  // namespace

// --- frames ---------------------------------------------------------------

std::pair<double, double> PadGeometry::normalise(int x, int y) const {
    const double nx = static_cast<double>(x - x_min) / std::max(1, x_max - x_min);
    const double ny = static_cast<double>(y - y_min) / std::max(1, y_max - y_min);
    return {std::min(1.0, std::max(0.0, nx)), std::min(1.0, std::max(0.0, ny))};
}

std::optional<std::pair<double, double>> ContactFrame::centroid() const {
    if (count <= 0) return std::nullopt;
    // Compensated (Neumaier) summation, because that is what the Python build
    // this replaces actually does: since 3.12 ``sum()`` over floats carries a
    // running error term, so a naive loop here would disagree in the last
    // couple of bits and every progress value would drift (~1e-16 relative).
    // That is invisible on screen but it is enough to move a swipe that sits
    // exactly on the half-card boundary to the neighbouring window, so the
    // arithmetic is matched rather than merely approximated.
    double sx = 0.0;
    double sy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    for (int i = 0; i < count; ++i) {
        const Contact& contact = contacts[static_cast<std::size_t>(i)];
        const double tx = sx + contact.x;
        cx += (std::abs(sx) >= std::abs(contact.x)) ? (sx - tx) + contact.x
                                                    : (contact.x - tx) + sx;
        sx = tx;
        const double ty = sy + contact.y;
        cy += (std::abs(sy) >= std::abs(contact.y)) ? (sy - ty) + contact.y
                                                    : (contact.y - ty) + sy;
        sy = ty;
    }
    return std::make_pair((sx + cx) / count, (sy + cy) / count);
}

ContactFrame make_frame(double time, std::initializer_list<Contact> contacts, bool dropped,
                        int btn_fingers) {
    ContactFrame frame;
    frame.time = time;
    frame.dropped = dropped;
    frame.btn_fingers = btn_fingers;
    for (const Contact& contact : contacts) {
        if (frame.count >= kMaxContacts) break;
        frame.contacts[static_cast<std::size_t>(frame.count++)] = contact;
    }
    return frame;
}

// --- device discovery -----------------------------------------------------

std::vector<std::map<std::string, std::string>> parse_input_devices(const std::string& text) {
    std::vector<std::map<std::string, std::string>> blocks;
    std::map<std::string, std::string> current;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (trim(line).empty()) {
            if (!current.empty()) {
                blocks.push_back(current);
                current.clear();
            }
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        current[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
    }
    if (!current.empty()) blocks.push_back(current);
    return blocks;
}

TouchpadDevice find_touchpad(const std::optional<std::string>& name) {
    std::ifstream handle("/proc/bus/input/devices", std::ios::binary);
    if (!handle) {
        throw std::runtime_error("cannot read /proc/bus/input/devices");
    }
    const std::string text((std::istreambuf_iterator<char>(handle)), std::istreambuf_iterator<char>());

    std::optional<TouchpadDevice> fallback;
    for (const auto& block : parse_input_devices(text)) {
        const auto name_it = block.find("N");
        if (name_it == block.end()) continue;
        std::string device_name = name_it->second;
        if (device_name.rfind("Name=\"", 0) == 0) device_name = device_name.substr(6);
        if (!device_name.empty() && device_name.back() == '"') device_name.pop_back();

        const auto handlers_it = block.find("H");
        if (handlers_it == block.end()) continue;
        const auto node = event_node(handlers_it->second);
        if (!node) continue;

        if (name) {
            if (lower(device_name).find(lower(*name)) != std::string::npos) {
                return TouchpadDevice{*node, device_name};
            }
            continue;
        }
        if (lower(device_name).find("touchpad") != std::string::npos && !fallback) {
            fallback = TouchpadDevice{*node, device_name};
        }
    }
    if (fallback) return *fallback;
    if (name) throw std::runtime_error("no input device matching '" + *name + "' found");
    throw std::runtime_error("no touchpad found in /proc/bus/input/devices");
}

// --- EvdevSource ----------------------------------------------------------

EvdevSource::EvdevSource(std::string node, std::string name, bool exclusive)
    : node_(std::move(node)), name_(std::move(name)) {
    int flags = O_RDONLY;
    if (!exclusive) flags |= O_NONBLOCK;
    fd_ = ::open(node_.c_str(), flags);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + node_);
    }
    if (exclusive) {
        const int current = fcntl(fd_, F_GETFL, 0);
        if (current >= 0) fcntl(fd_, F_SETFL, current | O_NONBLOCK);
    }
    geometry_ = read_geometry(fd_);
    buffer_.reserve(24 * 128);
}

EvdevSource::~EvdevSource() { close(); }

void EvdevSource::close() {
    if (closed_) return;
    closed_ = true;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void EvdevSource::apply_abs(int code, int value) {
    if (code == ABS_MT_SLOT) {
        if (value >= 0 && value < static_cast<int>(slots_.size())) current_slot_ = value;
        return;
    }
    const std::size_t slot_index = static_cast<std::size_t>(current_slot_);
    if (slot_index >= slots_.size()) return;
    Slot& slot = slots_[slot_index];
    if (code == ABS_MT_TRACKING_ID) {
        if (value == -1) {
            slot = Slot{};
        } else {
            slot.active = true;
        }
    } else if (code == ABS_MT_POSITION_X) {
        slot.x = value;
    } else if (code == ABS_MT_POSITION_Y) {
        slot.y = value;
    }
}

ContactFrame EvdevSource::build_frame(double now) const {
    ContactFrame frame;
    frame.time = now;
    frame.btn_fingers = btn_fingers_;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.active || frame.count >= kMaxContacts) continue;
        const auto [nx, ny] = geometry_.normalise(slot.x, slot.y);
        frame.contacts[static_cast<std::size_t>(frame.count++)] =
            Contact{static_cast<int>(index), nx, ny};
    }
    return frame;
}

std::vector<ContactFrame> EvdevSource::decode(const unsigned char* data, std::size_t length) {
    std::vector<ContactFrame> frames;
    const double now = monotonic_now();
    buffer_.insert(buffer_.end(), data, data + length);

    const std::size_t record = sizeof(input_event);
    std::size_t offset = 0;
    for (; offset + record <= buffer_.size(); offset += record) {
        input_event event{};
        std::memcpy(&event, buffer_.data() + offset, record);
        if (event.type == EV_ABS) {
            apply_abs(event.code, event.value);
        } else if (event.type == EV_KEY) {
            if (is_finger_button(event.code)) {
                btn_fingers_ = event.value ? fingers_for_button(event.code) : 0;
            }
        } else if (event.type == EV_SYN) {
            if (event.code == SYN_REPORT) {
                frames.push_back(build_frame(now));
            } else if (event.code == SYN_DROPPED) {
                for (auto& slot : slots_) slot = Slot{};
                ContactFrame dropped;
                dropped.time = now;
                dropped.dropped = true;
                dropped.btn_fingers = btn_fingers_;
                frames.push_back(dropped);
            }
        }
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    return frames;
}

std::vector<ContactFrame> EvdevSource::read_frames(double timeout) {
    std::vector<ContactFrame> frames;
    if (closed_ || fd_ < 0) return frames;

    pollfd watched{fd_, POLLIN, 0};
    const int timeout_ms = static_cast<int>(timeout * 1000.0);
    int ready = 0;
    do {
        ready = ::poll(&watched, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return frames;

    unsigned char buffer[24 * 128];
    while (true) {
        const ssize_t read_bytes = ::read(fd_, buffer, sizeof(buffer));
        if (read_bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == ENODEV || errno == EIO) {
                throw DeviceLost(std::strerror(errno));
            }
            break;
        }
        if (read_bytes == 0) break;
        auto decoded = decode(buffer, static_cast<std::size_t>(read_bytes));
        frames.insert(frames.end(), decoded.begin(), decoded.end());
    }
    return frames;
}

// --- TouchpadReader -------------------------------------------------------

TouchpadReader::TouchpadReader(FrameHandler on_frames, std::optional<std::string> name,
                               DeviceHandler on_device_change, double reconnect_delay)
    : on_frames_(std::move(on_frames)),
      name_(std::move(name)),
      on_device_change_(std::move(on_device_change)),
      reconnect_delay_(reconnect_delay) {}

TouchpadReader::~TouchpadReader() { stop(); }

void TouchpadReader::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void TouchpadReader::stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
            return;
        }
        thread_.join();
    }
}

bool TouchpadReader::running() const { return thread_.joinable() && !stop_.load(); }

void TouchpadReader::report(const std::string& message, const PadGeometry* geometry) {
    if (on_device_change_) {
        try {
            on_device_change_(message, geometry);
        } catch (...) {
            // reporting must never kill the reader
        }
    }
}

void TouchpadReader::run() {
    while (!stop_.load()) {
        std::optional<TouchpadDevice> device;
        try {
            device = find_touchpad(name_);
        } catch (const std::exception& exc) {
            report(std::string("touchpad unavailable (") + exc.what() + "); retrying", nullptr);
            for (int i = 0; i < 20 && !stop_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(reconnect_delay_ * 50)));
            }
            continue;
        }

        try {
            EvdevSource source(device->node, device->name);
            report("reading " + device->name + " at " + device->node, &source.geometry());
            while (!stop_.load()) {
                auto frames = source.read_frames(0.5);
                if (frames.empty()) continue;
                try {
                    on_frames_(frames);
                } catch (const std::exception& exc) {
                    report(std::string("frame handler error: ") + exc.what(), nullptr);
                } catch (...) {
                    report("frame handler error", nullptr);
                }
            }
        } catch (const DeviceLost& exc) {
            report(std::string("device lost (") + exc.what() + "); reconnecting", nullptr);
        } catch (const std::exception& exc) {
            report(std::string("touchpad unavailable (") + exc.what() + "); retrying", nullptr);
        }
        for (int i = 0; i < 20 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(reconnect_delay_ * 50)));
        }
    }
}

// --- recording and replay -------------------------------------------------

std::string format_frames(const std::vector<ContactFrame>& frames) {
    std::ostringstream out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i) out << '\n';
        const ContactFrame& frame = frames[i];
        out << "{\"t\": " << python_float(std::round(frame.time * 1e6) / 1e6) << ", \"c\": [";
        for (int c = 0; c < frame.count; ++c) {
            const Contact& contact = frame.contacts[static_cast<std::size_t>(c)];
            if (c) out << ", ";
            out << '[' << contact.slot << ", " << python_float(std::round(contact.x * 1e5) / 1e5)
                << ", " << python_float(std::round(contact.y * 1e5) / 1e5) << ']';
        }
        out << "], \"dropped\": " << (frame.dropped ? "true" : "false") << '}';
    }
    return out.str();
}

bool parse_frame_record(const std::string& line, ContactFrame& out) {
    if (line.empty() || line[0] == '#') return false;
    const std::string trimmed = trim(line);
    if (trimmed.empty()) return false;

    JsonCursor cursor(trimmed);
    double stamp = 0.0;
    if (!cursor.seek("t") || !cursor.number(stamp)) return false;

    ContactFrame frame;
    frame.time = stamp;
    if (!cursor.seek("c")) return false;
    if (!cursor.character('[')) return false;
    if (!cursor.character(']')) {
        while (true) {
            if (!cursor.character('[')) return false;
            int slot = 0;
            double x = 0.0;
            double y = 0.0;
            if (!cursor.integer(slot) || !cursor.character(',') || !cursor.number(x) ||
                !cursor.character(',') || !cursor.number(y) || !cursor.character(']')) {
                return false;
            }
            if (frame.count < kMaxContacts) {
                frame.contacts[static_cast<std::size_t>(frame.count++)] = Contact{slot, x, y};
            }
            if (cursor.character(',')) continue;
            if (cursor.character(']')) break;
            return false;
        }
    }

    if (cursor.seek("dropped")) {
        double ignored = 0.0;
        if (cursor.literal("true")) {
            frame.dropped = true;
        } else if (cursor.literal("false")) {
            frame.dropped = false;
        } else if (cursor.number(ignored)) {
            frame.dropped = ignored != 0.0;
        }
    }
    out = frame;
    return true;
}

void replay_stream(std::istream& in, double max_sleep,
                   const std::function<void(const ContactFrame&)>& on_frame) {
    std::string line;
    std::optional<double> previous;
    while (std::getline(in, line)) {
        ContactFrame frame;
        if (!parse_frame_record(line, frame)) continue;
        if (previous) {
            const double delay = frame.time - *previous;
            if (delay > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(std::min(delay, max_sleep)));
            }
        }
        previous = frame.time;
        on_frame(frame);
    }
}

}  // namespace wintab
