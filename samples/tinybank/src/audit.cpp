// tinybank - audit trail
#include "tinybank/audit.h"

#include "tinybank/log.h"
#include "tinybank/sha256.h"

namespace tinybank {

std::string audit_fingerprint(const std::string& actor,
                              const std::string& action,
                              const std::string& detail) {
    return sha256_hex(actor + "|" + action + "|" + detail);
}

AuditLog::AuditLog(std::size_t maxEntries) : maxEntries_(maxEntries) {}

void AuditLog::record(const std::string& actor,
                      const std::string& action,
                      const std::string& detail) {
    AuditEntry e;
    e.actor = actor;
    e.action = action;
    e.detail = detail;
    e.fingerprint = audit_fingerprint(actor, action, detail);
    entries_.push_back(e);

    if (entries_.size() > maxEntries_) {
        entries_.erase(entries_.begin());
        log_warn("audit: ring buffer full, oldest entry dropped");
    }
}

void AuditLog::clear() { entries_.clear(); }

std::string AuditLog::chain_digest() const {
    std::string acc = "genesis";
    for (const AuditEntry& e : entries_) {
        acc = sha256_hex(acc + e.fingerprint);
    }
    return acc;
}

}  // namespace tinybank
