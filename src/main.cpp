// Command line entry point.
//
//     wintab                 switch apps with three fingers
//     wintab --probe 20      confirm the pad reports three contacts
//     wintab --dry-run       read the pad, print decisions, draw nothing
//     wintab --replay log.jsonl   re-run a recorded swipe
//     wintab --record log.jsonl   record frames while running
//     wintab --print-config  show every tunable and where it comes from
#include <X11/Xlib.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "app.hpp"
#include "config.hpp"
#include "log.hpp"
#include "touchpad.hpp"

using namespace wintab;

namespace {

constexpr const char* kVersion = "0.1.0";

struct Args {
    std::optional<std::string> config;
    std::optional<std::string> device;
    std::optional<double> probe;
    std::optional<std::string> replay;
    std::optional<std::string> record;
    bool invert = false;
    bool fps = false;
    bool verbose = false;
    bool dry_run = false;
    bool print_config = false;
    bool version = false;
    bool trace_events = false;
    bool check_conflicts = false;
};

void usage(FILE* out) {
    std::fprintf(out,
                 "usage: wintab [-h] [--config CONFIG] [--device DEVICE] [--invert] [--fps]\n"
                 "              [-v] [--dry-run] [--probe [SECONDS]] [--replay FILE]\n"
                 "              [--record FILE] [--print-config] [--version]\n"
                 "\n"
                 "Windows 11 style three-finger app switching for X11/Cinnamon.\n");
}

bool parse_double(const std::string& text, double& out) {
    try {
        std::size_t used = 0;
        out = std::stod(text, &used);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

bool parse(int argc, char** argv, Args& args, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::optional<std::string> inline_value;
        const std::size_t equals = arg.find('=');
        if (arg.rfind("--", 0) == 0 && equals != std::string::npos) {
            inline_value = arg.substr(equals + 1);
            arg = arg.substr(0, equals);
        }
        const auto take_value = [&](std::optional<std::string>& target) {
            if (inline_value) {
                target = *inline_value;
                return true;
            }
            if (i + 1 >= argc) {
                error = "argument " + arg + ": expected one argument";
                return false;
            }
            target = argv[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            usage(stdout);
            std::exit(0);
        } else if (arg == "--version") {
            args.version = true;
        } else if (arg == "--print-config") {
            args.print_config = true;
        } else if (arg == "--invert") {
            args.invert = true;
        } else if (arg == "--fps") {
            args.fps = true;
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "--dry-run") {
            args.dry_run = true;
        } else if (arg == "--trace-events") {
            args.trace_events = true;
        } else if (arg == "--check-conflicts") {
            // Installer support: report what else wants these three fingers and
            // exit, without touching the pad or the screen.
            args.check_conflicts = true;
        } else if (arg == "--config") {
            std::optional<std::string> value;
            if (!take_value(value)) return false;
            args.config = *value;
        } else if (arg == "--device") {
            std::optional<std::string> value;
            if (!take_value(value)) return false;
            args.device = *value;
        } else if (arg == "--replay") {
            std::optional<std::string> value;
            if (!take_value(value)) return false;
            args.replay = *value;
        } else if (arg == "--record") {
            std::optional<std::string> value;
            if (!take_value(value)) return false;
            args.record = *value;
        } else if (arg == "--probe") {
            double seconds = 20.0;
            if (inline_value) {
                if (!parse_double(*inline_value, seconds)) {
                    error = "argument --probe: invalid float value: '" + *inline_value + "'";
                    return false;
                }
            } else if (i + 1 < argc) {
                double candidate = 0.0;
                if (parse_double(argv[i + 1], candidate)) {
                    seconds = candidate;
                    ++i;
                }
            }
            args.probe = seconds;
        } else {
            error = "unrecognized arguments: " + arg;
            return false;
        }
    }
    return true;
}

const char* describe(const ContactFrame& frame) {
    static thread_local std::string line;
    if (frame.count == 0) {
        line = "no contacts";
        return line.c_str();
    }
    char buffer[256];
    std::string spots;
    for (int i = 0; i < frame.count; ++i) {
        std::snprintf(buffer, sizeof(buffer), "%s%.3f,%.3f", i ? "  " : "",
                      frame.contacts[static_cast<std::size_t>(i)].x,
                      frame.contacts[static_cast<std::size_t>(i)].y);
        spots += buffer;
    }
    line = std::to_string(frame.count) + " contact(s): " + spots;
    return line.c_str();
}

volatile sig_atomic_t g_probe_stop = 0;
void on_probe_signal(int) { g_probe_stop = 1; }

//: Live view of the pad: the one thing to run before trusting the gesture.
int probe(double seconds, const std::optional<std::string>& device) {
    TouchpadDevice found;
    try {
        found = find_touchpad(device);
    } catch (const std::exception& exc) {
        log_line(std::string("wintab: ") + exc.what());
        log_line("wintab: is your user in the 'input' group?  groups | grep input");
        return 1;
    }

    std::unique_ptr<EvdevSource> source;
    try {
        source = std::make_unique<EvdevSource>(found.node, found.name);
    } catch (const std::exception&) {
        log_line("wintab: permission denied opening " + found.node);
        log_line("wintab: add yourself to the 'input' group and log back in:");
        log_line("        sudo usermod -aG input $USER");
        return 1;
    }

    const PadGeometry geometry = source->geometry();
    log_line("wintab: reading " + found.name);
    log_line("wintab: node " + found.node + "  range " + std::to_string(geometry.x_min) + ".." +
             std::to_string(geometry.x_max) + " x " + std::to_string(geometry.y_min) + ".." +
             std::to_string(geometry.y_max));
    char header[128];
    std::snprintf(header, sizeof(header),
                  "wintab: watch for %.0fs -- put three fingers down and drag left and right",
                  std::max(1.0, seconds));
    log_line(header);
    log_line("");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(std::max(1.0, seconds));
    int best = 0;
    int frames = 0;
    std::string last_line;
    signal(SIGINT, on_probe_signal);
    while (std::chrono::steady_clock::now() < deadline && !g_probe_stop) {
        std::vector<ContactFrame> batch;
        try {
            batch = source->read_frames(0.5);
        } catch (const DeviceLost& exc) {
            log_line("");
            log_line(std::string("wintab: device lost (") + exc.what() + ")");
            break;
        }
        for (const ContactFrame& frame : batch) {
            frames += 1;
            best = std::max(best, frame.count);
            const std::string line = describe(frame);
            if (line != last_line) {
                std::printf("\r%-72s", line.c_str());
                std::fflush(stdout);
                last_line = line;
            }
        }
    }
    source->close();
    signal(SIGINT, SIG_DFL);

    std::printf("\n");
    std::printf("\n");
    log_line("wintab: frames read: " + std::to_string(frames) +
             ", most contacts seen at once: " + std::to_string(best));
    if (best >= 3) {
        log_line("wintab: PASS -- the pad reports three simultaneous contacts.");
        return 0;
    }
    log_line(
        "wintab: FAIL -- fewer than three contacts were seen.\n"
        "wintab: a three-finger gesture cannot work on this pad; check that you\n"
        "wintab: really touched it during the run, and that no other gesture\n"
        "wintab: tool (touchegg) is swallowing the touches first.");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    // Before any Xlib call: the capture thread and the pointer lock each open
    // their own Display.
    XInitThreads();

    Args args;
    std::string error;
    if (!parse(argc, argv, args, error)) {
        std::fprintf(stderr, "wintab: error: %s\n", error.c_str());
        usage(stderr);
        return 2;
    }

    const std::optional<std::filesystem::path> config_file =
        args.config ? std::optional<std::filesystem::path>(*args.config) : std::nullopt;
    wintab::Config config = wintab::load_config(config_file);
    if (args.device) config.touchpad_name = *args.device;
    if (args.invert) config.invert_direction = !config.invert_direction;
    if (args.fps) config.show_fps = true;
    if (args.verbose) config.verbose = true;

    if (args.print_config) {
        wintab::print_config(config, config_file);
        return 0;
    }
    if (args.version) {
        std::printf("wintab %s\n", kVersion);
        return 0;
    }
    if (args.probe) return probe(*args.probe, config.touchpad_name);

    if (args.check_conflicts) {
        for (const std::string& warning : wintab::find_conflicts()) {
            std::printf("%s\n", warning.c_str());
        }
        return 0;
    }

    for (const std::string& warning : wintab::find_conflicts()) {
        wintab::log_line("wintab: WARNING: " + warning);
    }

    try {
        wintab::WinTab app(config, !args.dry_run, args.verbose, args.record);
        app.set_trace_events(args.trace_events);
        if (args.replay) app.replay(*args.replay);
        return app.run();
    } catch (const std::exception& exc) {
        wintab::log_line(std::string("wintab: ") + exc.what());
        return 1;
    }
}
