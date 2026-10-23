// An in-memory XBackend, so the window-model rules stay testable without a
// server -- the same role the Python tests' SimpleNamespace fakes played.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "x11.hpp"

namespace testing {

class FakeBackend : public wintab::XBackend {
public:
    using PropertyEvent = wintab::PropertyEvent;

    wintab::AtomId intern(const std::string& name) override {
        if (broken_) throw std::runtime_error("broken connection");
        const auto found = ids_.find(name);
        if (found != ids_.end()) return found->second;
        const wintab::AtomId id = next_id_++;
        ids_.emplace(name, id);
        names_.emplace(id, name);
        return id;
    }

    std::string atom_name(wintab::AtomId atom) override {
        const auto found = names_.find(atom);
        return found == names_.end() ? std::string() : found->second;
    }

    wintab::PropertyValue property(wintab::WindowId window, wintab::AtomId atom) override {
        const auto found = properties_.find({window, atom_name(atom)});
        if (found == properties_.end()) return {};
        return found->second;
    }

    std::optional<wintab::WindowAttributes> attributes(wintab::WindowId window) override {
        const auto found = override_redirect_.find(window);
        return wintab::WindowAttributes{found != override_redirect_.end() && found->second};
    }

    std::optional<wintab::Geometry> geometry(wintab::WindowId window) override {
        const auto found = geometry_.find(window);
        if (found == geometry_.end()) return std::nullopt;
        return found->second;
    }

    std::optional<wintab::Point> root_position(wintab::WindowId window) override {
        const auto found = positions_.find(window);
        if (found == positions_.end()) return std::nullopt;
        return found->second;
    }

    wintab::WindowId parent_of(wintab::WindowId window) override {
        const auto found = parents_.find(window);
        return found == parents_.end() ? 0 : found->second;
    }

    void change_cardinal(wintab::WindowId window, wintab::AtomId atom,
                         unsigned long value) override {
        cardinals_.emplace_back(window, atom_name(atom), value);
        wintab::PropertyValue stored;
        stored.format = 32;
        stored.numbers = {value};
        properties_[{window, atom_name(atom)}] = stored;
    }

    void send_state_message(wintab::WindowId window, int action, wintab::AtomId first,
                            wintab::AtomId second) override {
        state_messages_.emplace_back(window, action, atom_name(first), atom_name(second));
    }

    void send_activate(wintab::WindowId window) override { activated_.push_back(window); }

    void watch_property_events() override { watching_ = true; }

    wintab::WindowId root_window() override { return root_; }

    bool pending_events() override { return !events_.empty(); }

    bool next_property_event(PropertyEvent& out) override {
        if (events_.empty()) return false;
        out = events_.front();
        events_.pop_front();
        return true;
    }

    int fileno() override { return -1; }

    wintab::Rect screen_geometry() override { return screen_; }

    // -- fixtures ----------------------------------------------------------
    //: Make every atom intern (and so every request) fail, as a dropped X
    //: connection does: callers must report this, not crash.
    void set_broken(bool broken) { broken_ = broken; }

    void set_root(wintab::WindowId root) { root_ = root; }
    void set_screen(const wintab::Rect& screen) { screen_ = screen; }

    void set_numbers(wintab::WindowId window, const std::string& name,
                     const std::vector<unsigned long>& values) {
        wintab::PropertyValue stored;
        stored.format = 32;
        stored.numbers = values;
        properties_[{window, name}] = stored;
    }

    void set_root_numbers(const std::string& name, const std::vector<unsigned long>& values) {
        set_numbers(root_, name, values);
    }

    void set_bytes(wintab::WindowId window, const std::string& name, const std::string& bytes) {
        wintab::PropertyValue stored;
        stored.format = 8;
        stored.bytes = bytes;
        properties_[{window, name}] = stored;
    }

    void set_root_bytes(const std::string& name, const std::string& bytes) {
        set_bytes(root_, name, bytes);
    }

    void set_override_redirect(wintab::WindowId window, bool value) {
        override_redirect_[window] = value;
    }

    void set_geometry(wintab::WindowId window, const wintab::Geometry& geometry) {
        geometry_[window] = geometry;
    }

    void set_root_position(wintab::WindowId window, std::optional<wintab::Point> position) {
        if (position) {
            positions_[window] = *position;
        } else {
            positions_.erase(window);
        }
    }

    void set_parent(wintab::WindowId window, wintab::WindowId parent) { parents_[window] = parent; }

    void push_property_event(const std::string& atom, double time = 0.0) {
        events_.push_back(PropertyEvent{intern(atom), time});
    }

    const std::vector<wintab::WindowId>& activated() const { return activated_; }
    const std::vector<std::tuple<wintab::WindowId, int, std::string, std::string>>&
    state_messages() const {
        return state_messages_;
    }
    const std::vector<std::tuple<wintab::WindowId, std::string, unsigned long>>& cardinals() const {
        return cardinals_;
    }
    bool watching() const { return watching_; }

private:
    wintab::WindowId root_ = 1;
    wintab::Rect screen_{0, 0, 1920, 1080};
    wintab::AtomId next_id_ = 100;
    std::map<std::string, wintab::AtomId> ids_;
    std::map<wintab::AtomId, std::string> names_;
    std::map<std::pair<wintab::WindowId, std::string>, wintab::PropertyValue> properties_;
    std::map<wintab::WindowId, bool> override_redirect_;
    std::map<wintab::WindowId, wintab::Geometry> geometry_;
    std::map<wintab::WindowId, wintab::Point> positions_;
    std::map<wintab::WindowId, wintab::WindowId> parents_;
    std::deque<PropertyEvent> events_;
    std::vector<wintab::WindowId> activated_;
    bool broken_ = false;
    std::vector<std::tuple<wintab::WindowId, int, std::string, std::string>> state_messages_;
    std::vector<std::tuple<wintab::WindowId, std::string, unsigned long>> cardinals_;
    bool watching_ = false;
};

}  // namespace testing
