// tinybank - key/value configuration loader
// Layer: configuration. Sits between the raw text and every service.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace tinybank {

class Config {
public:
    // Parse "key = value" lines. Blank lines and '#' comments are ignored.
    static bool parse(const std::string& text, Config& out);

    std::string get(const std::string& key, const std::string& fallback = "") const;
    int get_int(const std::string& key, int fallback) const;
    bool has(const std::string& key) const;
    std::vector<std::string> keys() const;
    std::size_t size() const;

private:
    std::map<std::string, std::string> values_;
};

// Validate a config for required keys; returns the list of missing keys.
std::vector<std::string> config_missing_keys(const Config& config,
                                             const std::vector<std::string>& required);

// Convenience: parse and validate in one step, logging the outcome.
bool load_config(const std::string& text,
                 const std::vector<std::string>& required,
                 Config& out);

}  // namespace tinybank
