// tinybank - account and transfer domain
#include "tinybank/account_service.h"

#include "tinybank/log.h"

namespace tinybank {

const char* transfer_status_name(TransferStatus status) {
    switch (status) {
        case TransferStatus::Ok: return "ok";
        case TransferStatus::UnknownSource: return "unknown_source";
        case TransferStatus::UnknownTarget: return "unknown_target";
        case TransferStatus::InsufficientFunds: return "insufficient_funds";
        case TransferStatus::InvalidAmount: return "invalid_amount";
    }
    return "unknown";
}

AccountService::AccountService() {}

bool AccountService::open_account(const std::string& id,
                                  const std::string& owner,
                                  const Money& opening) {
    if (accounts_.count(id)) {
        log_warn("account_service: duplicate account " + id);
        return false;
    }
    Account a;
    a.id = id;
    a.owner = owner;
    a.balance = opening;
    accounts_[id] = a;
    log_info("account_service: opened " + id + " with " + opening.format());
    return true;
}

const Account* AccountService::get_account(const std::string& id) const {
    const auto it = accounts_.find(id);
    return it == accounts_.end() ? nullptr : &it->second;
}

std::vector<Account> AccountService::balance_snapshot() const {
    std::vector<Account> out;
    out.reserve(accounts_.size());
    for (const auto& kv : accounts_) out.push_back(kv.second);
    return out;
}

TransferStatus AccountService::transfer(const std::string& from,
                                        const std::string& to,
                                        const Money& amount,
                                        AuditLog& audit) {
    auto src = accounts_.find(from);
    if (src == accounts_.end()) {
        log_warn("account_service: unknown source " + from);
        return TransferStatus::UnknownSource;
    }
    auto dst = accounts_.find(to);
    if (dst == accounts_.end()) {
        log_warn("account_service: unknown target " + to);
        return TransferStatus::UnknownTarget;
    }
    if (amount.cents() <= 0) {
        log_warn("account_service: non-positive transfer amount");
        return TransferStatus::InvalidAmount;
    }
    if (src->second.balance.cents() < amount.cents()) {
        log_warn("account_service: insufficient funds on " + from);
        return TransferStatus::InsufficientFunds;
    }

    src->second.balance = src->second.balance - amount;
    dst->second.balance = dst->second.balance + amount;
    audit.record(from, "transfer", to + ":" + amount.format());
    log_info("account_service: moved " + amount.format() + " from " + from + " to " + to);
    return TransferStatus::Ok;
}

Money AccountService::total_balance() const {
    Money total = Money::from_cents(0);
    for (const auto& kv : accounts_) {
        total = total + kv.second.balance;
    }
    return total;
}

std::string AccountService::statement_line(const std::string& accountId) const {
    const Account* a = get_account(accountId);
    if (a == nullptr) return "unknown account";
    return a->id + "  " + a->owner + "  " + a->balance.format();
}

}  // namespace tinybank
