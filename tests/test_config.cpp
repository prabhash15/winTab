#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "config.hpp"
#include "support/harness.hpp"
#include "toml_lite.hpp"

using namespace wintab;

namespace {

std::filesystem::path scratch(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("wintab-test-" + name);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream handle(path, std::ios::binary);
    handle << text;
}

}  // namespace

TEST_CASE(config_defaults_match_the_python_build) {
    Config config;
    CHECK_NEAR(config.step_fraction, 0.095, 1e-12);
    CHECK_NEAR(config.dead_zone_fraction, 0.025, 1e-12);
    CHECK_NEAR(config.commit_fraction, 0.5, 1e-12);
    CHECK_NEAR(config.arm_window_ms, 40.0, 1e-12);
    CHECK_EQ(config.band_pad_px, 26);
    CHECK_EQ(config.order, std::string("recent"));
    CHECK(!config.group_by_app);
    CHECK(!config.touchpad_name.has_value());
    CHECK_NEAR(config.highlight_rgb[1], 0.47, 1e-12);
}

TEST_CASE(config_a_missing_file_is_the_defaults) {
    const auto dir = scratch("missing");
    const Config config = load_config(dir / "nope.toml");
    CHECK_NEAR(config.step_fraction, 0.095, 1e-12);
    CHECK_EQ(config.order, std::string("recent"));
}

TEST_CASE(config_reads_values_types_and_arrays) {
    const auto dir = scratch("values");
    const auto path = dir / "config.toml";
    write_file(path,
               "# a comment\n"
               "step_fraction = 0.2\n"
               "commit_fraction = 0.35\n"
               "highlight_rgb = [1.0, 0.0, 0.5]\n"
               "order = \"opened\"\n"
               "group_by_app = true\n"
               "band_pad_px = 40\n"
               "touchpad_name = \"HTIX5288\"\n"
               "invert_direction = false\n");
    const Config config = load_config(path);
    CHECK_NEAR(config.step_fraction, 0.2, 1e-12);
    CHECK_NEAR(config.commit_fraction, 0.35, 1e-12);
    CHECK_NEAR(config.highlight_rgb[0], 1.0, 1e-12);
    CHECK_NEAR(config.highlight_rgb[2], 0.5, 1e-12);
    CHECK_EQ(config.order, std::string("opened"));
    CHECK(config.group_by_app);
    CHECK_EQ(config.band_pad_px, 40);
    CHECK(config.touchpad_name && *config.touchpad_name == "HTIX5288");
}

TEST_CASE(config_unknown_keys_are_ignored_and_known_ones_still_apply) {
    const auto dir = scratch("unknown");
    const auto path = dir / "config.toml";
    write_file(path, "nonsense = 1\nstep_fraction = 0.31\n");
    const Config config = load_config(path);
    CHECK_NEAR(config.step_fraction, 0.31, 1e-12);
}

TEST_CASE(config_a_bad_order_falls_back_to_recent) {
    const auto dir = scratch("order");
    const auto path = dir / "config.toml";
    write_file(path, "order = \"sideways\"\n");
    const Config config = load_config(path);
    CHECK_EQ(config.order, std::string("recent"));
}

TEST_CASE(config_a_malformed_file_keeps_every_default) {
    const auto dir = scratch("malformed");
    const auto path = dir / "config.toml";
    write_file(path, "step_fraction = \n");
    const Config config = load_config(path);
    CHECK_NEAR(config.step_fraction, 0.095, 1e-12);
    CHECK_EQ(config.order, std::string("recent"));
}

TEST_CASE(config_tables_are_reported_as_malformed) {
    const auto dir = scratch("table");
    const auto path = dir / "config.toml";
    write_file(path, "[gesture]\nstep_fraction = 0.2\n");
    const Config config = load_config(path);
    CHECK_NEAR(config.step_fraction, 0.095, 1e-12);
}

TEST_CASE(config_a_wrong_type_keeps_the_default) {
    const auto dir = scratch("type");
    const auto path = dir / "config.toml";
    write_file(path, "step_fraction = \"fast\"\n");
    const Config config = load_config(path);
    CHECK_NEAR(config.step_fraction, 0.095, 1e-12);
}

TEST_CASE(config_highlight_color_is_clamped_and_padded) {
    Config config;
    config.highlight_rgb = {2.0, -1.0, 0.5};
    const auto colour = config.highlight_color();
    CHECK_NEAR(colour[0], 1.0, 1e-12);
    CHECK_NEAR(colour[1], 0.0, 1e-12);
    CHECK_NEAR(colour[2], 0.5, 1e-12);
}

TEST_CASE(config_python_float_matches_repr) {
    CHECK_EQ(python_float(0.4), std::string("0.4"));
    CHECK_EQ(python_float(40.0), std::string("40.0"));
    CHECK_EQ(python_float(0.095), std::string("0.095"));
    CHECK_EQ(python_float(8.0), std::string("8.0"));
    CHECK_EQ(python_float(0.0), std::string("0.0"));
}

TEST_CASE(toml_lite_parses_the_shipped_grammar) {
    const auto parsed = toml_lite::parse(
        "# comment\n"
        "a = 1\n"
        "b = 2.5\n"
        "c = true\n"
        "d = \"text with # hash\"\n"
        "e = [0.0, 0.47, 0.83]\n");
    CHECK(parsed.ok);
    CHECK_EQ(parsed.entries.size(), std::size_t(5));
    CHECK_EQ(parsed.entries[0].key, std::string("a"));
    CHECK_EQ(parsed.entries[0].value.integer, 1LL);
    CHECK_NEAR(parsed.entries[1].value.number, 2.5, 1e-12);
    CHECK(parsed.entries[2].value.boolean);
    CHECK_EQ(parsed.entries[3].value.string, std::string("text with # hash"));
    CHECK_EQ(parsed.entries[4].value.array.size(), std::size_t(3));
    CHECK_NEAR(parsed.entries[4].value.array[1], 0.47, 1e-12);
}
