#include "toml_lite.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace wintab::toml_lite {
namespace {

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// Cut a trailing comment, but never inside a quoted string.
std::string strip_comment(const std::string& line) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

bool parse_string(const std::string& text, std::string& out, std::string& error) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        error = "unterminated string";
        return false;
    }
    out.clear();
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        const char ch = text[i];
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 2 > text.size()) {
            error = "dangling escape in string";
            return false;
        }
        const char next = text[++i];
        switch (next) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            default:
                error = std::string("unsupported escape \\") + next;
                return false;
        }
    }
    return true;
}

bool parse_number(const std::string& text, Value& value, std::string& error) {
    if (text.empty()) {
        error = "missing value";
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long as_int = std::strtoll(text.c_str(), &end, 10);
    if (end && *end == '\0' && errno == 0) {
        value.type = Value::Type::Integer;
        value.integer = as_int;
        return true;
    }
    errno = 0;
    end = nullptr;
    const double as_float = std::strtod(text.c_str(), &end);
    if (end && *end == '\0' && errno == 0) {
        value.type = Value::Type::Float;
        value.number = as_float;
        return true;
    }
    error = "not a number: " + text;
    return false;
}

bool parse_array(const std::string& text, Value& value, std::string& error) {
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') {
        error = "unterminated array";
        return false;
    }
    value.type = Value::Type::Array;
    std::string body = text.substr(1, text.size() - 2);
    // Split on commas that are not inside a string.
    std::vector<std::string> parts;
    std::string current;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : body) {
        if (in_string) {
            current.push_back(ch);
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            current.push_back(ch);
        } else if (ch == ',') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);

    for (const std::string& raw : parts) {
        const std::string item = trim(raw);
        if (item.empty()) continue;
        Value inner;
        std::string inner_error;
        if (item.front() == '"') {
            error = "arrays of strings are not supported";
            return false;
        }
        if (item.front() == '[') {
            error = "nested arrays are not supported";
            return false;
        }
        if (!parse_number(item, inner, inner_error)) {
            error = "array item " + inner_error;
            return false;
        }
        value.array.push_back(inner.as_double());
    }
    return true;
}

bool parse_value(const std::string& text, Value& value, std::string& error) {
    if (text.empty()) {
        error = "missing value";
        return false;
    }
    if (text.front() == '"') {
        if (!parse_string(text, value.string, error)) return false;
        value.type = Value::Type::String;
        return true;
    }
    if (text.front() == '[') return parse_array(text, value, error);
    if (text == "true") {
        value.type = Value::Type::Boolean;
        value.boolean = true;
        return true;
    }
    if (text == "false") {
        value.type = Value::Type::Boolean;
        value.boolean = false;
        return true;
    }
    if (text.front() == '{') {
        error = "inline tables are not supported";
        return false;
    }
    return parse_number(text, value, error);
}

}  // namespace

Result parse(const std::string& text) {
    Result result;
    std::istringstream stream(text);
    std::string line;
    int number = 0;
    bool in_table = false;
    while (std::getline(stream, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string stripped = trim(strip_comment(line));
        if (stripped.empty()) continue;
        if (stripped.front() == '[') {
            // ``[table]`` or ``[[array of tables]]``: remember the name for the
            // loader's warning, then ignore the keys underneath it.
            std::size_t begin = 1;
            std::size_t end = stripped.size();
            while (begin < end && stripped[begin] == '[') ++begin;
            while (end > begin && (stripped[end - 1] == ']' || stripped[end - 1] == ' ')) --end;
            const std::string name = trim(stripped.substr(begin, end - begin));
            if (name.empty()) {
                result.ok = false;
                result.error = "empty table name (line " + std::to_string(number) + ")";
                return result;
            }
            result.tables.push_back(name);
            in_table = true;
            continue;
        }
        if (in_table) continue;
        const std::size_t equals = stripped.find('=');
        if (equals == std::string::npos) {
            result.ok = false;
            result.error = "expected key = value (line " + std::to_string(number) + ")";
            return result;
        }
        const std::string key = trim(stripped.substr(0, equals));
        const std::string raw_value = trim(stripped.substr(equals + 1));
        if (key.empty()) {
            result.ok = false;
            result.error = "empty key (line " + std::to_string(number) + ")";
            return result;
        }
        for (const char ch : key) {
            const bool valid = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
            if (!valid) {
                result.ok = false;
                result.error = "bad key '" + key + "' (line " + std::to_string(number) + ")";
                return result;
            }
        }
        Value value;
        std::string error;
        if (!parse_value(raw_value, value, error)) {
            result.ok = false;
            result.error = error + " (line " + std::to_string(number) + ")";
            return result;
        }
        result.entries.push_back(Entry{key, value, number});
    }
    return result;
}

}  // namespace wintab::toml_lite
