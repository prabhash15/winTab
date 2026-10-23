// Replay a recorded swipe through the real gesture engine and print one line
// per event. Used to diff this implementation against the Python one on the
// same recording: no X, no GTK, no overlay.
//
//     trace_replay [--config PATH] FILE
//
// Lines: B <t> | U <progress> <steps> <t> | E <progress> <velocity> <steps> <t>
//        C <reason> <t> | T <t>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#include "config.hpp"
#include "gesture.hpp"
#include "touchpad.hpp"

using namespace wintab;

namespace {

void print_event(const GestureEvent& event) {
    std::visit(
        [](const auto& value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, SwipeBegin>) {
                std::printf("B %.17g\n", value.time);
            } else if constexpr (std::is_same_v<Type, SwipeUpdate>) {
                std::printf("U %.17g %d %.17g\n", value.progress, value.steps, value.time);
            } else if constexpr (std::is_same_v<Type, SwipeEnd>) {
                std::printf("E %.17g %.17g %d %.17g\n", value.progress, value.velocity,
                            value.steps, value.time);
            } else if constexpr (std::is_same_v<Type, SwipeCancel>) {
                std::printf("C %s %.17g\n", value.reason.c_str(), value.time);
            } else {
                std::printf("T %.17g\n", value.time);
            }
        },
        event);
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> config_file;
    std::string replay_file;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_file = std::filesystem::path(argv[++i]);
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "trace_replay: unknown option %s\n", arg.c_str());
            return 2;
        } else {
            replay_file = arg;
        }
    }
    if (replay_file.empty()) {
        std::fprintf(stderr, "usage: trace_replay [--config PATH] FILE\n");
        return 2;
    }

    const Config config = load_config(config_file);
    ThreeFingerSwipe engine(GestureParams::from_config(config));

    std::ifstream handle(replay_file);
    if (!handle) {
        std::fprintf(stderr, "trace_replay: cannot read %s\n", replay_file.c_str());
        return 1;
    }
    replay_stream(handle, 0.0, [&engine](const ContactFrame& frame) {
        for (const GestureEvent& event : engine.feed(frame)) print_event(event);
    });
    return 0;
}
