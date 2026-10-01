// tinybank - string helpers
// Layer: infrastructure. Widely reused => high fan-in, so changes here have
// a large blast radius. This is intentional in the demo repository.
#pragma once

#include <string>
#include <vector>

namespace tinybank {

std::string trim(const std::string& s);
std::string to_lower(const std::string& s);
std::string to_upper(const std::string& s);
std::vector<std::string> split(const std::string& s, char delimiter);
bool starts_with(const std::string& s, const std::string& prefix);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string pad_left(const std::string& s, std::size_t width, char fill);

// Header-inline helper: header-defined bodies like this are common in real C++
// libraries and are exactly what the "defs in public headers" metric counts.
inline bool is_blank(const std::string& s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
    }
    return true;
}

}  // namespace tinybank
