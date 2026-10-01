// tinybank - JSON-ish field extraction
// Layer: parsing. Deliberately not a full JSON parser (this is a demo repo);
// it shows the kind of narrow utility that many layers end up depending on.
#pragma once

#include <string>
#include <vector>

namespace tinybank {

// Extract a top-level string field: {"name": "value"} -> value.
// Returns empty when the key is absent.
std::string json_extract_string(const std::string& json, const std::string& key);

// Extract a top-level number field as text.
std::string json_extract_number(const std::string& json, const std::string& key);

// Split a JSON array of numbers "[1, 2, 3]" into its textual elements.
std::vector<std::string> json_array_elements(const std::string& json,
                                             const std::string& key);

}  // namespace tinybank
