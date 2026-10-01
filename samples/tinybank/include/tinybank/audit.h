// tinybank - audit trail
// Layer: cross-cutting. Records every state change with a tamper-evident hash.
#pragma once

#include <string>
#include <vector>

namespace tinybank {

struct AuditEntry {
    std::string actor;
    std::string action;
    std::string detail;
    std::string fingerprint;
};

class AuditLog {
public:
    explicit AuditLog(std::size_t maxEntries = 128);

    void record(const std::string& actor,
                const std::string& action,
                const std::string& detail);

    const std::vector<AuditEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    void clear();

    // Hash chain over all entries; changes when any earlier entry changes.
    std::string chain_digest() const;

private:
    std::vector<AuditEntry> entries_;
    std::size_t maxEntries_;
};

// Compute the fingerprint used for a single audit record.
std::string audit_fingerprint(const std::string& actor,
                              const std::string& action,
                              const std::string& detail);

}  // namespace tinybank
