// A very small JSON reader -- only what the generated golden fixtures use:
// objects with string keys, arrays, numbers, booleans and null. Written here
// because no JSON library is installed, and the fixture files are ours (tools/
// make_fixtures.py writes them), so the grammar this needs is fixed.
#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing {

struct Json {
    enum class Kind { Null, Boolean, Number, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    bool is_object() const { return kind == Kind::Object; }
    bool is_array() const { return kind == Kind::Array; }

    const Json& at(const std::string& key) const {
        const auto found = object.find(key);
        if (found == object.end()) throw std::runtime_error("missing key: " + key);
        return found->second;
    }

    double num(const std::string& key) const { return at(key).number; }
    int integer(const std::string& key) const { return static_cast<int>(at(key).number); }

    std::vector<double> numbers(const std::string& key) const {
        std::vector<double> out;
        for (const Json& item : at(key).array) out.push_back(item.number);
        return out;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    Json parse() {
        Json value;
        parse_value(value);
        skip_space();
        if (pos_ != text_.size()) throw std::runtime_error("trailing data in fixture");
        return value;
    }

private:
    void skip_space() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char take() {
        skip_space();
        if (pos_ >= text_.size()) throw std::runtime_error("unexpected end of fixture");
        return text_[pos_++];
    }

    void expect(char wanted) {
        const char got = take();
        if (got != wanted) throw std::runtime_error(std::string("expected ") + wanted);
    }

    void parse_value(Json& out) {
        const char next = take();
        switch (next) {
            case '{': out.kind = Json::Kind::Object; parse_object(out); return;
            case '[': out.kind = Json::Kind::Array; parse_array(out); return;
            case 't': out.kind = Json::Kind::Boolean; out.boolean = true; return;
            case 'f': out.kind = Json::Kind::Boolean; out.boolean = false; return;
            case 'n': out.kind = Json::Kind::Null; return;
            case '"': throw std::runtime_error("string values are not used in fixtures");
            default: out.kind = Json::Kind::Number; parse_number(next, out); return;
        }
    }

    void parse_number(char first, Json& out) {
        std::string buffer(1, first);
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            const bool part = std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '-' ||
                              c == '+' || c == '.' || c == 'e' || c == 'E';
            if (!part) break;
            buffer.push_back(c);
            ++pos_;
        }
        out.number = std::strtod(buffer.c_str(), nullptr);
    }

    void parse_object(Json& out) {
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return;
        }
        while (true) {
            expect('"');
            std::string key;
            while (pos_ < text_.size() && text_[pos_] != '"') key.push_back(text_[pos_++]);
            if (pos_ >= text_.size()) throw std::runtime_error("unterminated key");
            ++pos_;  // closing quote
            expect(':');
            Json value;
            parse_value(value);
            out.object.emplace(std::move(key), std::move(value));
            const char next = take();
            if (next == '}') return;
            if (next != ',') throw std::runtime_error("expected , or } in object");
        }
    }

    void parse_array(Json& out) {
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return;
        }
        while (true) {
            Json value;
            parse_value(value);
            out.array.push_back(std::move(value));
            const char next = take();
            if (next == ']') return;
            if (next != ',') throw std::runtime_error("expected , or ] in array");
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

}  // namespace testing
