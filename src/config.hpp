// Configuration: every tunable, its default, and the TOML loader.
//
// Field order matches wintab/config.py's dataclass exactly, because
// --print-config prints in declaration order and must stay diffable against the
// Python build.
#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace wintab {

struct Config {
    // --- gesture feel -----------------------------------------------------
    double step_fraction = 0.095;
    double dead_zone_fraction = 0.025;
    double direction_dominance = 1.15;
    double commit_fraction = 0.5;
    double rubber_band = 0.35;
    bool invert_direction = false;
    double arm_window_ms = 40.0;
    double tap_max_ms = 300.0;
    double tap_max_move = 0.015;

    // --- carousel layout --------------------------------------------------
    double slot_fraction = 0.22;
    double card_gap_fraction = 0.018;
    double max_card_height_fraction = 0.40;
    double band_alpha = 0.12;
    int band_pad_px = 26;
    double card_radius = 8.0;
    double card_shadow_px = 26.0;
    double dim_others = 0.18;

    // --- the selection outline -------------------------------------------
    std::array<double, 3> highlight_rgb{0.0, 0.47, 0.83};
    double highlight_px = 3.0;
    double highlight_gap_px = 4.0;
    int highlight_ms = 120;
    double caption_font_px = 13.0;
    double caption_gap_px = 6.0;
    double row_parallax = 0.0;

    // --- visuals ----------------------------------------------------------
    double dim = 0.30;
    double blur_radius = 6.0;
    int blur_downscale = 8;
    int fade_ms = 120;

    // --- model ------------------------------------------------------------
    bool group_by_app = false;
    std::string order = "recent";

    // --- capture ----------------------------------------------------------
    double capture_debounce_s = 0.4;
    double capture_poll_s = 5.0;

    // --- input ------------------------------------------------------------
    std::optional<std::string> touchpad_name;
    bool freeze_pointer = false;

    // --- debug ------------------------------------------------------------
    bool verbose = false;
    bool show_fps = false;

    //: ``highlight_rgb`` as a clamped 3-tuple, so a typo cannot crash a draw.
    std::array<double, 3> highlight_color() const;
};

//: $XDG_CONFIG_HOME/wintab/config.toml, else ~/.config/wintab/config.toml.
std::filesystem::path config_path();

//: Load config.toml, ignoring unknown keys so old files keep working.
Config load_config(const std::optional<std::filesystem::path>& path = std::nullopt);

//: Print the effective configuration in the Python build's exact format.
void print_config(const Config& config, const std::optional<std::filesystem::path>& path);

//: A float the way Python's repr() writes it: shortest round-trip, always with
//: a decimal point or an exponent (``40`` -> ``40.0``).
std::string python_float(double value);

}  // namespace wintab
