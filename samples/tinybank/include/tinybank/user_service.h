// tinybank - user directory
// Layer: service. Depends on config, strings and logging.
#pragma once

#include <string>
#include <vector>

#include "tinybank/config.h"

namespace tinybank {

struct User {
    std::string id;
    std::string name;
    std::string email;
    std::string role;
};

class UserService {
public:
    explicit UserService(const Config& config);

    // Parse a "id:name:email:role" record and add it. Returns false when the
    // record is malformed.
    bool add_from_record(const std::string& record);

    const User* find_user(const std::string& id) const;
    std::vector<User> all_users() const;
    std::size_t user_count() const;

    std::string normalize_email(const std::string& email) const;

private:
    const Config& config_;
    std::vector<User> users_;
};

}  // namespace tinybank
