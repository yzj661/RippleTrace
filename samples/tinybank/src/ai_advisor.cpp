// tinybank - AI advisor facade
#include "tinybank/ai_advisor.h"

#include "tinybank/http_client.h"
#include "tinybank/json.h"
#include "tinybank/log.h"
#include "tinybank/sha256.h"
#include "tinybank/strings.h"

namespace tinybank {

std::string build_advice_prompt(const AdviceRequest& request,
                                const UserService& users,
                                const AccountService& accounts) {
    std::string prompt = "You are a banking assistant.\n";

    const User* user = users.find_user(request.user_id);
    if (user != nullptr) {
        prompt += "User: " + user->name + " (" + user->role + ")\n";
    } else {
        prompt += "User: unknown\n";
    }

    prompt += "Accounts:\n";
    prompt += accounts.statement_line(request.user_id) + "\n";
    prompt += "Question: " + request.question + "\n";
    prompt += "Answer concisely.";
    return prompt;
}

std::string parse_model_response(const std::string& raw) {
    const std::string answer = json_extract_string(raw, "answer");
    if (!answer.empty()) return answer;
    // Fall back to the whole body when the endpoint returns plain text.
    return trim(raw);
}

AdviceService::AdviceService(const Config& config,
                             const UserService& users,
                             const AccountService& accounts,
                             AuditLog& audit)
    : config_(config), users_(users), accounts_(accounts), audit_(audit) {}

AdviceResult AdviceService::advise(const AdviceRequest& request) const {
    AdviceResult result;

    if (trim(request.question).empty()) {
        result.error = "empty question";
        log_warn("ai_advisor: rejected empty question");
        return result;
    }

    const std::string prompt = build_advice_prompt(request, users_, accounts_);
    result.request_fingerprint = sha256_hex(prompt);
    result.model = config_.get("ai.model", "local-demo-model");

    const std::string endpoint = config_.get("ai.endpoint", "https://api.example.invalid");
    HttpClient client(endpoint);

    const std::string raw = client.get_json_field("/v1/complete", "answer");
    if (raw.empty()) {
        result.error = "empty model response";
        log_error("ai_advisor: model returned nothing");
        return result;
    }

    result.answer = parse_model_response(raw);
    result.ok = true;
    audit_.record(request.user_id, "ai_advice", result.request_fingerprint);
    log_info("ai_advisor: answered via " + result.model);
    return result;
}

}  // namespace tinybank
