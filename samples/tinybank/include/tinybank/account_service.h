// tinybank - account and transfer domain
// Layer: domain service. Depends on Money (primitive) and the audit trail.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "tinybank/audit.h"
#include "tinybank/money.h"

namespace tinybank {

struct Account {
    std::string id;
    std::string owner;
    Money balance;
};

enum class TransferStatus { Ok, UnknownSource, UnknownTarget, InsufficientFunds, InvalidAmount };

const char* transfer_status_name(TransferStatus status);

class AccountService {
public:
    AccountService();

    bool open_account(const std::string& id, const std::string& owner, const Money& opening);

    const Account* get_account(const std::string& id) const;
    std::size_t account_count() const { return accounts_.size(); }

    // Snapshot of all accounts, ordered by id.
    std::vector<Account> balance_snapshot() const;

    // Validate and apply a transfer, recording it in the audit log.
    TransferStatus transfer(const std::string& from,
                            const std::string& to,
                            const Money& amount,
                            AuditLog& audit);

    // Sum of all balances (used by the reconciliation report).
    Money total_balance() const;

    std::string statement_line(const std::string& accountId) const;

private:
    std::map<std::string, Account> accounts_;
};

}  // namespace tinybank
