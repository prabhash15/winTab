#include "config.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>

#include "log.hpp"
#include "toml_lite.hpp"

namespace wintab {
namespace {

constexpr std::string_view kOrderModes[] = {"opened", "recent"};

std::string quote(const std::string& text) { return "'" + text + "'"; }

bool number_to_double(const toml_lite::Value& value, double& out) {
    if (!value.is_number()) return false;
    out = value.as_double();
    return true;
}

bool number_to_int(const toml_lite::Value& value, int& out) {
    if (!value.is_number()) return false;
    out = static_cast<int>(value.as_double());
    return true;
}

// One key -> one field. A wrong type is reported and the default kept; the
// Python build would store it and fail later, which is worse.
bool assign(Config& cfg, const toml_lite::Entry& entry) {
    const std::string& key = entry.key;
    const toml_lite::Value& value = entry.value;
    auto warn_type = [&](const char* expected) -> bool {
        log_line("wintab: config key " + quote(key) + " must be " + expected + "; ignored");
        return false;
    };

    auto dbl = [&](double& field) -> bool {
        double parsed = 0.0;
        if (!number_to_double(value, parsed)) return warn_type("a number");
        field = parsed;
        return true;
    };
    auto integer = [&](int& field) -> bool {
        int parsed = 0;
        if (!number_to_int(value, parsed)) return warn_type("a whole number");
        field = parsed;
        return true;
    };
    auto boolean = [&](bool& field) -> bool {
        if (value.type != toml_lite::Value::Type::Boolean) return warn_type("true or false");
        field = value.boolean;
        return true;
    };

    if (key == "step_fraction") return dbl(cfg.step_fraction);
    if (key == "dead_zone_fraction") return dbl(cfg.dead_zone_fraction);
    if (key == "direction_dominance") return dbl(cfg.direction_dominance);
    if (key == "commit_fraction") return dbl(cfg.commit_fraction);
    if (key == "rubber_band") return dbl(cfg.rubber_band);
    if (key == "invert_direction") return boolean(cfg.invert_direction);
    if (key == "arm_window_ms") return dbl(cfg.arm_window_ms);
    if (key == "tap_max_ms") return dbl(cfg.tap_max_ms);
    if (key == "tap_max_move") return dbl(cfg.tap_max_move);

    if (key == "slot_fraction") return dbl(cfg.slot_fraction);
    if (key == "card_gap_fraction") return dbl(cfg.card_gap_fraction);
    if (key == "max_card_height_fraction") return dbl(cfg.max_card_height_fraction);
    if (key == "band_alpha") return dbl(cfg.band_alpha);
    if (key == "band_pad_px") return integer(cfg.band_pad_px);
    if (key == "card_radius") return dbl(cfg.card_radius);
    if (key == "card_shadow_px") return dbl(cfg.card_shadow_px);
    if (key == "dim_others") return dbl(cfg.dim_others);

    if (key == "highlight_rgb") {
        if (value.type != toml_lite::Value::Type::Array) return warn_type("an array of numbers");
        std::array<double, 3> colours{0.0, 0.0, 0.0};
        for (std::size_t i = 0; i < 3 && i < value.array.size(); ++i) colours[i] = value.array[i];
        cfg.highlight_rgb = colours;
        return true;
    }
    if (key == "highlight_px") return dbl(cfg.highlight_px);
    if (key == "highlight_gap_px") return dbl(cfg.highlight_gap_px);
    if (key == "highlight_ms") return integer(cfg.highlight_ms);
    if (key == "caption_font_px") return dbl(cfg.caption_font_px);
    if (key == "caption_gap_px") return dbl(cfg.caption_gap_px);
    if (key == "row_parallax") return dbl(cfg.row_parallax);

    if (key == "dim") return dbl(cfg.dim);
    if (key == "blur_radius") return dbl(cfg.blur_radius);
    if (key == "blur_downscale") return integer(cfg.blur_downscale);
    if (key == "fade_ms") return integer(cfg.fade_ms);

    if (key == "group_by_app") return boolean(cfg.group_by_app);
    if (key == "order") {
        if (value.type != toml_lite::Value::Type::String) return warn_type("a string");
        bool known = false;
        for (const auto mode : kOrderModes) known = known || value.string == mode;
        if (!known) {
            log_line("wintab: order " + quote(value.string) + " is not one of opened/recent; using " +
                     quote(cfg.order));
            return true;
        }
        cfg.order = value.string;
        return true;
    }

    if (key == "capture_debounce_s") return dbl(cfg.capture_debounce_s);
    if (key == "capture_poll_s") return dbl(cfg.capture_poll_s);

    if (key == "touchpad_name") {
        if (value.type != toml_lite::Value::Type::String) return warn_type("a string");
        cfg.touchpad_name = value.string;
        return true;
    }
    if (key == "freeze_pointer") return boolean(cfg.freeze_pointer);

    if (key == "verbose") return boolean(cfg.verbose);
    if (key == "show_fps") return boolean(cfg.show_fps);

    log_line("wintab: unknown config key " + quote(key) + " ignored");
    return false;
}

}  // namespace

std::array<double, 3> Config::highlight_color() const {
    std::array<double, 3> out{0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < 3; ++i) {
        const double raw = highlight_rgb[i];
        out[i] = std::min(1.0, std::max(0.0, raw));
    }
    return out;
}

std::filesystem::path config_path() {
    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = std::filesystem::path(home) / ".config";
    } else {
        base = ".config";
    }
    return base / "wintab" / "config.toml";
}

Config load_config(const std::optional<std::filesystem::path>& path_opt) {
    Config cfg;
    const std::filesystem::path path = path_opt.value_or(config_path());

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return cfg;

    std::ifstream handle(path, std::ios::binary);
    if (!handle) {
        log_line("wintab: ignoring " + path.string() + ": cannot read file");
        return cfg;
    }
    const std::string text((std::istreambuf_iterator<char>(handle)), std::istreambuf_iterator<char>());

    const toml_lite::Result parsed = toml_lite::parse(text);
    if (!parsed.ok) {
        log_line("wintab: ignoring " + path.string() + ": " + parsed.error);
        return cfg;
    }
    // tomllib handed the Python loader a table as one key (its value a dict),
    // which the loader reported as unknown and skipped. Same warning here, and
    // the flat keys around the table still apply -- a file written as
    // ``[touchpad]`` must not lose every other setting.
    for (const std::string& table : parsed.tables) {
        log_line("wintab: unknown config key " + quote(table.substr(0, table.find('.'))) +
                 " ignored");
    }
    for (const auto& entry : parsed.entries) assign(cfg, entry);
    return cfg;
}

std::string python_float(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc()) return "0.0";
    std::string text(buffer, result.ptr);
    const bool has_point = text.find('.') != std::string::npos;
    const bool has_exponent = text.find('e') != std::string::npos || text.find('E') != std::string::npos;
    if (!has_point && !has_exponent) text += ".0";
    return text;
}

void print_config(const Config& config, const std::optional<std::filesystem::path>& path_opt) {
    const std::filesystem::path source = path_opt.value_or(config_path());
    std::error_code ec;
    if (std::filesystem::exists(source, ec)) {
        log_line("# effective configuration (from " + source.string() + ")");
    } else {
        log_line("# built-in defaults (no config file at " + source.string() + ")");
    }

    const auto colour = [](const std::array<double, 3>& rgb) {
        std::string text = "[";
        for (std::size_t i = 0; i < 3; ++i) {
            if (i) text += ", ";
            text += python_float(rgb[i]);
        }
        return text + "]";
    };
    const auto boolean = [](bool value) { return value ? "true" : "false"; };

    log_line("step_fraction = " + python_float(config.step_fraction));
    log_line("dead_zone_fraction = " + python_float(config.dead_zone_fraction));
    log_line("direction_dominance = " + python_float(config.direction_dominance));
    log_line("commit_fraction = " + python_float(config.commit_fraction));
    log_line("rubber_band = " + python_float(config.rubber_band));
    log_line("invert_direction = " + std::string(boolean(config.invert_direction)));
    log_line("arm_window_ms = " + python_float(config.arm_window_ms));
    log_line("tap_max_ms = " + python_float(config.tap_max_ms));
    log_line("tap_max_move = " + python_float(config.tap_max_move));

    log_line("slot_fraction = " + python_float(config.slot_fraction));
    log_line("card_gap_fraction = " + python_float(config.card_gap_fraction));
    log_line("max_card_height_fraction = " + python_float(config.max_card_height_fraction));
    log_line("band_alpha = " + python_float(config.band_alpha));
    log_line("band_pad_px = " + std::to_string(config.band_pad_px));
    log_line("card_radius = " + python_float(config.card_radius));
    log_line("card_shadow_px = " + python_float(config.card_shadow_px));
    log_line("dim_others = " + python_float(config.dim_others));

    log_line("highlight_rgb = " + colour(config.highlight_rgb));
    log_line("highlight_px = " + python_float(config.highlight_px));
    log_line("highlight_gap_px = " + python_float(config.highlight_gap_px));
    log_line("highlight_ms = " + std::to_string(config.highlight_ms));
    log_line("caption_font_px = " + python_float(config.caption_font_px));
    log_line("caption_gap_px = " + python_float(config.caption_gap_px));
    log_line("row_parallax = " + python_float(config.row_parallax));

    log_line("dim = " + python_float(config.dim));
    log_line("blur_radius = " + python_float(config.blur_radius));
    log_line("blur_downscale = " + std::to_string(config.blur_downscale));
    log_line("fade_ms = " + std::to_string(config.fade_ms));

    log_line("group_by_app = " + std::string(boolean(config.group_by_app)));
    log_line("order = \"" + config.order + "\"");

    log_line("capture_debounce_s = " + python_float(config.capture_debounce_s));
    log_line("capture_poll_s = " + python_float(config.capture_poll_s));

    if (config.touchpad_name) {
        log_line("touchpad_name = \"" + *config.touchpad_name + "\"");
    } else {
        log_line("touchpad_name = None");
    }
    log_line("freeze_pointer = " + std::string(boolean(config.freeze_pointer)));

    log_line("verbose = " + std::string(boolean(config.verbose)));
    log_line("show_fps = " + std::string(boolean(config.show_fps)));
}

}  // namespace wintab
