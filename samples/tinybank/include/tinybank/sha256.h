// tinybank - SHA-256 digest
// Layer: crypto primitive. Used by the fingerprint, audit and token paths, so
// even a one-line change fans out across several layers. The demo patch makes
// exactly this kind of change.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tinybank {

// Raw 32-byte digest.
std::vector<std::uint8_t> sha256_bytes(const std::string& data);

// Lowercase hexadecimal digest (64 chars).
std::string sha256_hex(const std::string& data);

}  // namespace tinybank
