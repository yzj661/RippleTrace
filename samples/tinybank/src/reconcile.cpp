// tinybank - end-of-day reconciliation report
#include "tinybank/reconcile.h"

#include "tinybank/log.h"
#include "tinybank/strings.h"

namespace tinybank {

ReconciliationReport build_reconciliation(const AccountService& accounts,
                                          const AuditLog& audit) {
    ReconciliationReport report;

    const Money total = accounts.total_balance();
    report.total = total.format();
    report.audit_digest = audit.chain_digest();

    // statement_line() is reached through get_account(), so a change to
    // account lookup or formatting propagates all the way up to here.
    for (const Account& a : accounts.balance_snapshot()) {
        ReconciliationRow row;
        row.account_id = a.id;
        row.owner = a.owner;
        row.balance = accounts.statement_line(a.id);
        report.rows.push_back(row);
    }

    report.balanced = true;   // decided by verify_reconciliation()
    log_info("reconcile: built report with " + std::to_string(report.rows.size()) +
             " row(s)");
    return report;
}

std::string render_reconciliation(const ReconciliationReport& report) {
    std::string out = "=== reconciliation ===\n";
    for (const ReconciliationRow& row : report.rows) {
        out += pad_left(row.account_id, 10, ' ') + "  " + row.balance + "\n";
    }
    out += "total: " + report.total + "\n";
    out += "audit: " + report.audit_digest + "\n";
    out += report.balanced ? "status: balanced\n" : "status: UNBALANCED\n";
    return out;
}

bool verify_reconciliation(const ReconciliationReport& report,
                           const AccountService& accounts) {
    const Money recomputed = accounts.total_balance();
    const bool match = (recomputed.format() == report.total);
    if (!match) {
        log_error("reconcile: total mismatch, expected " + report.total +
                  " but recomputed " + recomputed.format());
    }
    return match;
}

}  // namespace tinybank
