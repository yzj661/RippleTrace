// tinybank - JSON-ish field extraction
#include "tinybank/json.h"

#include "tinybank/strings.h"

namespace tinybank {

namespace {

// Locate `"key"` and return the offset of the value start, or npos.
std::size_t find_value(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != ':') return std::string::npos;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    return pos;
}

}  // namespace

std::string json_extract_string(const std::string& json, const std::string& key) {
    std::size_t pos = find_value(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '"') return std::string();
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
        out.push_back(json[pos]);
        ++pos;
    }
    return out;
}

std::string json_extract_number(const std::string& json, const std::string& key) {
    std::size_t pos = find_value(json, key);
    if (pos == std::string::npos) return std::string();
    std::string out;
    while (pos < json.size() && (std::isdigit(static_cast<unsigned char>(json[pos])) ||
                                 json[pos] == '-' || json[pos] == '.' ||
                                 json[pos] == 'e' || json[pos] == 'E' || json[pos] == '+')) {
        out.push_back(json[pos]);
        ++pos;
    }
    return out;
}

std::vector<std::string> json_array_elements(const std::string& json,
                                             const std::string& key) {
    std::vector<std::string> out;
    std::size_t pos = find_value(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '[') return out;
    std::size_t end = json.find(']', pos);
    if (end == std::string::npos) return out;
    const std::string body = json.substr(pos + 1, end - pos - 1);
    for (const std::string& piece : split(body, ',')) {
        const std::string t = trim(piece);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

}  // namespace tinybank
