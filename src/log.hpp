// Small logging shim: the Python build prints user-facing messages with print(),
// and those messages are documented, so they are reproduced verbatim.
#pragma once

#include <cstdio>
#include <string>

namespace wintab {

inline void log_line(const std::string& message) {
    std::fputs(message.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

inline void log_raw(const std::string& text) {
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

}  // namespace wintab
