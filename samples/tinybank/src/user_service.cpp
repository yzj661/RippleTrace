// tinybank - user directory
#include "tinybank/user_service.h"

#include "tinybank/log.h"
#include "tinybank/strings.h"

namespace tinybank {

UserService::UserService(const Config& config) : config_(config) {
    const std::string defaultRole = config_.get("user.default_role", "member");
    log_debug("user_service: default role is " + defaultRole);
}

std::string UserService::normalize_email(const std::string& email) const {
    return to_lower(trim(email));
}

bool UserService::add_from_record(const std::string& record) {
    const std::vector<std::string> fields = split(record, ':');
    if (fields.size() != 4) {
        log_warn("user_service: rejecting malformed record");
        return false;
    }
    if (trim(fields[0]).empty()) {
        log_warn("user_service: rejecting record without id");
        return false;
    }
    if (find_user(trim(fields[0])) != nullptr) {
        log_warn("user_service: duplicate id " + trim(fields[0]));
        return false;
    }

    User u;
    u.id = trim(fields[0]);
    u.name = trim(fields[1]);
    u.email = normalize_email(fields[2]);
    u.role = trim(fields[3]).empty() ? config_.get("user.default_role", "member")
                                     : trim(fields[3]);
    users_.push_back(u);
    log_info("user_service: added user " + u.id);
    return true;
}

const User* UserService::find_user(const std::string& id) const {
    for (const User& u : users_) {
        if (u.id == id) return &u;
    }
    log_debug("user_service: lookup miss for " + id);
    return nullptr;
}

std::vector<User> UserService::all_users() const { return users_; }

std::size_t UserService::user_count() const { return users_.size(); }

}  // namespace tinybank
