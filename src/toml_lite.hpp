// A small TOML reader for wintab's config file.
//
// The Python build uses tomllib (TOML 1.0). The shipped config and every key in
// it are flat scalars plus one float array, so this parser implements exactly
// that grammar and reports anything else as a malformed file -- which the config
// loader turns into the same "ignoring <path>" warning tomllib would produce.
//
// Supported:
//   key = 1 | 1.5 | true | false | "text" | [1.0, 2.0]
//   # comments, blank lines, single-line strings with \" \\ \n \t escapes.
//
// Not supported (reported as an error, never silently ignored):
//   dotted keys, multi-line strings, dates and times.
// [tables] are reported through Result::tables and their keys skipped, because
// the Python loader saw them as a single unknown key and kept the rest of the
// file -- dropping every setting in the file would be a silent regression.
#pragma once

#include <string>
#include <vector>

namespace wintab::toml_lite {

struct Value {
    enum class Type { Integer, Float, Boolean, String, Array };
    Type type = Type::Integer;
    long long integer = 0;
    double number = 0.0;
    bool boolean = false;
    std::string string;
    std::vector<double> array;

    bool is_number() const { return type == Type::Integer || type == Type::Float; }
    double as_double() const { return type == Type::Integer ? static_cast<double>(integer) : number; }
};

struct Entry {
    std::string key;
    Value value;
    int line = 0;
};

struct Result {
    bool ok = true;
    std::string error;
    std::vector<Entry> entries;
    //: Table headers found in the file, in order. Their keys are skipped rather
    //: than the whole file being rejected: tomllib would hand the table to the
    //: loader as one (unknown) key, so the flat keys around it still apply.
    std::vector<std::string> tables;
};

Result parse(const std::string& text);

}  // namespace wintab::toml_lite
