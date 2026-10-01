// tinybank - end-of-day reconciliation report
// Layer: reporting. A deep consumer: it reaches the money primitive through
// the account service and the account service through the advisor path.
#pragma once

#include <string>
#include <vector>

#include "tinybank/account_service.h"
#include "tinybank/audit.h"
#include "tinybank/config.h"

namespace tinybank {

struct ReconciliationRow {
    std::string account_id;
    std::string owner;
    std::string balance;
};

struct ReconciliationReport {
    std::vector<ReconciliationRow> rows;
    std::string total;
    std::string audit_digest;
    bool balanced = false;
};

// Build the report for all accounts.
ReconciliationReport build_reconciliation(const AccountService& accounts,
                                          const AuditLog& audit);

// Render it as a fixed-width text block.
std::string render_reconciliation(const ReconciliationReport& report);

// True when the report's total matches the independently recomputed sum.
bool verify_reconciliation(const ReconciliationReport& report,
                           const AccountService& accounts);

}  // namespace tinybank
