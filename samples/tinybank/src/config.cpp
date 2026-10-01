// tinybank - key/value configuration loader
#include "tinybank/config.h"

#include "tinybank/log.h"
#include "tinybank/strings.h"

#include <cstdlib>

namespace tinybank {

bool Config::parse(const std::string& text, Config& out) {
    out.values_.clear();
    for (const std::string& rawLine : split(text, '\n')) {
        const std::string line = trim(rawLine);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            log_warn("config: ignoring malformed line: " + line);
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key.empty()) {
            log_warn("config: ignoring entry with empty key");
            continue;
        }
        out.values_[key] = value;
    }
    return true;
}

std::string Config::get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

int Config::get_int(const std::string& key, int fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        log_warn("config: value for '" + key + "' is not an integer");
        return fallback;
    }
}

bool Config::has(const std::string& key) const {
    return values_.find(key) != values_.end();
}

std::vector<std::string> Config::keys() const {
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (const auto& kv : values_) out.push_back(kv.first);
    return out;
}

std::size_t Config::size() const { return values_.size(); }

std::vector<std::string> config_missing_keys(const Config& config,
                                             const std::vector<std::string>& required) {
    std::vector<std::string> missing;
    for (const std::string& key : required) {
        if (!config.has(key)) missing.push_back(key);
    }
    return missing;
}

bool load_config(const std::string& text,
                 const std::vector<std::string>& required,
                 Config& out) {
    Config::parse(text, out);
    const std::vector<std::string> missing = config_missing_keys(out, required);
    if (!missing.empty()) {
        log_error("config: missing required key(s): " + join(missing, ", "));
        return false;
    }
    log_info("config: loaded " + std::to_string(out.size()) + " entry(ies)");
    return true;
}

}  // namespace tinybank
