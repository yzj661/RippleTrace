// tinybank - AI advisor facade
// Layer: application. Represents the "AI-assisted" surface of the product:
// it assembles a prompt from domain data, calls a model endpoint and parses
// the reply. Deliberately the deepest chain in the demo repo.
#pragma once

#include <string>
#include <vector>

#include "tinybank/account_service.h"
#include "tinybank/config.h"
#include "tinybank/user_service.h"

namespace tinybank {

struct AdviceRequest {
    std::string user_id;
    std::string question;
};

struct AdviceResult {
    bool ok = false;
    std::string answer;
    std::string model;
    std::string request_fingerprint;
    std::string error;
};

// Build the prompt sent to the model. Pure function => easy to test.
std::string build_advice_prompt(const AdviceRequest& request,
                               const UserService& users,
                               const AccountService& accounts);

// Pull the "answer" field out of a raw model response.
std::string parse_model_response(const std::string& raw);

class AdviceService {
public:
    AdviceService(const Config& config,
                  const UserService& users,
                  const AccountService& accounts,
                  AuditLog& audit);

    AdviceResult advise(const AdviceRequest& request) const;

private:
    const Config& config_;
    const UserService& users_;
    const AccountService& accounts_;
    AuditLog& audit_;
};

}  // namespace tinybank
