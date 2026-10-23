#include <sstream>
#include <string>
#include <vector>

#include "support/harness.hpp"
#include "touchpad.hpp"

using namespace wintab;

TEST_CASE(touchpad_geometry_normalises_and_clamps) {
    PadGeometry geometry;
    geometry.x_min = 0;
    geometry.x_max = 1000;
    geometry.y_min = 0;
    geometry.y_max = 500;
    const auto [nx, ny] = geometry.normalise(500, 250);
    CHECK_NEAR(nx, 0.5, 1e-12);
    CHECK_NEAR(ny, 0.5, 1e-12);
    const auto [cx, cy] = geometry.normalise(5000, -100);
    CHECK_NEAR(cx, 1.0, 1e-12);
    CHECK_NEAR(cy, 0.0, 1e-12);
}

TEST_CASE(touchpad_frames_count_and_centroid) {
    const auto frame = make_frame(1.0, {{0, 0.4, 0.5}, {1, 0.5, 0.5}, {2, 0.6, 0.5}});
    CHECK_EQ(frame.count, 3);
    const auto centre = frame.centroid();
    CHECK(centre.has_value());
    if (centre) {
        CHECK_NEAR(centre->first, 0.5, 1e-12);
        CHECK_NEAR(centre->second, 0.5, 1e-12);
    }
    CHECK(!make_frame(2.0, {}).centroid().has_value());
}

TEST_CASE(touchpad_frames_cap_at_eight_contacts) {
    std::initializer_list<Contact> many = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0},
                                           {4, 0, 0}, {5, 0, 0}, {6, 0, 0}, {7, 0, 0},
                                           {8, 0, 0}, {9, 0, 0}};
    CHECK_EQ(make_frame(0.0, many).count, kMaxContacts);
}

TEST_CASE(touchpad_parses_proc_input_devices) {
    const std::string text =
        "I: Bus=0018 Vendor=36b6 Product=c001 Version=0100\n"
        "N: Name=\"HTIX5288:00 36B6:C001 Touchpad\"\n"
        "P: Phys=i2c-HTIX5288:00\n"
        "H: Handlers=mouse1 event10 \n"
        "\n"
        "I: Bus=0011 Vendor=0001 Product=0001 Version=ab41\n"
        "N: Name=\"AT Translated Set 2 keyboard\"\n"
        "H: Handlers=sysrq kbd event3 leds \n"
        "\n";
    const auto blocks = parse_input_devices(text);
    CHECK_EQ(blocks.size(), std::size_t(2));
    CHECK_EQ(blocks[0].at("N"), std::string("Name=\"HTIX5288:00 36B6:C001 Touchpad\""));
    CHECK_EQ(blocks[0].at("H"), std::string("Handlers=mouse1 event10"));
}

TEST_CASE(touchpad_frame_records_round_trip) {
    const ContactFrame frame = make_frame(12.345678, {{0, 0.12345, 0.54321}, {1, 0.5, 0.5}}, true);
    const std::string line = format_frames({frame});
    CHECK(line.rfind("{\"t\": ", 0) == 0);
    CHECK(line.find("\"dropped\": true") != std::string::npos);
    CHECK(line.find("\"c\": [[0, 0.12345, 0.54321], [1, 0.5, 0.5]]") != std::string::npos);

    ContactFrame parsed;
    CHECK(parse_frame_record(line, parsed));
    CHECK_NEAR(parsed.time, 12.345678, 1e-6);
    CHECK_EQ(parsed.count, 2);
    CHECK(parsed.dropped);
    CHECK_NEAR(parsed.contacts[0].x, 0.12345, 1e-5);
    CHECK_NEAR(parsed.contacts[1].y, 0.5, 1e-5);
}

TEST_CASE(touchpad_a_record_without_contacts_round_trips) {
    const ContactFrame frame = make_frame(3.5, {});
    const std::string line = format_frames({frame});
    ContactFrame parsed;
    CHECK(parse_frame_record(line, parsed));
    CHECK_EQ(parsed.count, 0);
    CHECK(!parsed.dropped);
    CHECK_NEAR(parsed.time, 3.5, 1e-6);
}

TEST_CASE(touchpad_replay_stream_delivers_every_record) {
    const std::string text =
        "# a recorded swipe\n"
        "{\"t\": 0.0, \"c\": [[0, 0.4, 0.5], [1, 0.5, 0.5], [2, 0.6, 0.5]], \"dropped\": false}\n"
        "\n"
        "{\"t\": 0.01, \"c\": [[0, 0.41, 0.5], [1, 0.51, 0.5], [2, 0.61, 0.5]], \"dropped\": false}\n";
    std::istringstream stream(text);
    std::vector<ContactFrame> frames;
    replay_stream(stream, 0.0, [&frames](const ContactFrame& frame) { frames.push_back(frame); });
    CHECK_EQ(frames.size(), std::size_t(2));
    CHECK_EQ(frames.front().count, 3);
    CHECK_NEAR(frames.back().contacts[1].x, 0.51, 1e-5);
}

TEST_CASE(touchpad_bad_records_are_skipped) {
    ContactFrame parsed;
    CHECK(!parse_frame_record("", parsed));
    CHECK(!parse_frame_record("# comment", parsed));
    CHECK(!parse_frame_record("{\"t\": nope}", parsed));
    CHECK(!parse_frame_record("{\"c\": [[0, 0.1, 0.2]]}", parsed));
}
