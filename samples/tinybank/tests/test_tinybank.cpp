// tinybank - test suite
#include "tinybank/account_service.h"
#include "tinybank/ai_advisor.h"
#include "tinybank/audit.h"
#include "tinybank/config.h"
#include "tinybank/json.h"
#include "tinybank/money.h"
#include "tinybank/reconcile.h"
#include "tinybank/sha256.h"
#include "tinybank/strings.h"
#include "tinybank/user_service.h"

#include "test_framework.h"

namespace {

using namespace tinybank;

// ---------------------------------------------------------------
//  Primitive tests: these are the ones that protect sha256_hex,
//  which the demo patch modifies.
// ---------------------------------------------------------------
int test_sha256_empty() {
    TB_CASE("sha256 of empty string");
    const std::string digest = sha256_hex("");
    TB_CHECK_EQ(digest.size(), static_cast<std::size_t>(64));
    TB_CHECK_MSG(digest == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 "known FIPS vector for empty input");
    return 0;
}

int test_sha256_abc() {
    TB_CASE("sha256 of \"abc\"");
    const std::string digest = sha256_hex("abc");
    TB_CHECK_MSG(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 "known FIPS vector for abc");
    return 0;
}

int test_sha256_avalanche() {
    TB_CASE("sha256 avalanche behaviour");
    const std::string a = sha256_hex("tinybank");
    const std::string b = sha256_hex("tinybanl");
    TB_CHECK_EQ(a.size(), b.size());
    TB_CHECK_MSG(a != b, "one-bit input change must change the digest");
    return 0;
}

int test_strings_basics() {
    TB_CASE("string helpers");
    TB_CHECK_EQ(trim("  x  "), std::string("x"));
    TB_CHECK_EQ(to_lower("AbC"), std::string("abc"));
    TB_CHECK_EQ(to_upper("AbC"), std::string("ABC"));
    TB_CHECK(starts_with("tinybank", "tiny"));
    TB_CHECK(!starts_with("tiny", "tinybank"));
    const std::vector<std::string> parts = split("a:b:c", ':');
    TB_CHECK_EQ(parts.size(), static_cast<std::size_t>(3));
    TB_CHECK_EQ(join(parts, "-"), std::string("a-b-c"));
    TB_CHECK_EQ(pad_left("7", 3, '0'), std::string("007"));
    return 0;
}

int test_money_parsing() {
    TB_CASE("money parsing and formatting");
    Money m;
    TB_CHECK(Money::parse("10.25", m));
    TB_CHECK_EQ(m.cents(), static_cast<std::int64_t>(1025));
    TB_CHECK_EQ(m.format(), std::string("10.25"));

    TB_CHECK(Money::parse("-3.5", m));
    TB_CHECK_EQ(m.cents(), static_cast<std::int64_t>(-350));
    TB_CHECK_EQ(m.format(), std::string("-3.50"));
    TB_CHECK(m.is_negative());

    TB_CHECK(!Money::parse("abc", m));
    TB_CHECK(!Money::parse("", m));
    return 0;
}

int test_money_arithmetic() {
    TB_CASE("money arithmetic");
    const Money a = Money::from_cents(1000);
    const Money b = Money::from_cents(250);
    TB_CHECK_EQ((a + b).cents(), static_cast<std::int64_t>(1250));
    TB_CHECK_EQ((a - b).cents(), static_cast<std::int64_t>(750));
    TB_CHECK(a == Money::from_cents(1000));
    TB_CHECK_EQ(a.format_with_currency("CNY "), std::string("CNY 10.00"));
    return 0;
}

int test_config_parsing() {
    TB_CASE("config parsing and validation");
    Config config;
    Config::parse("# comment\nmode = strict\nlimit = 25\n\n", config);
    TB_CHECK_EQ(config.size(), static_cast<std::size_t>(2));
    TB_CHECK_EQ(config.get("mode"), std::string("strict"));
    TB_CHECK_EQ(config.get_int("limit", 0), 25);
    TB_CHECK_EQ(config.get("absent", "fallback"), std::string("fallback"));

    const std::vector<std::string> missing =
        config_missing_keys(config, {"mode", "audit.path"});
    TB_CHECK_EQ(missing.size(), static_cast<std::size_t>(1));
    TB_CHECK_EQ(missing.front(), std::string("audit.path"));
    return 0;
}

int test_json_extraction() {
    TB_CASE("json field extraction");
    const std::string body = "{\"name\": \"alice\", \"balance\": 1250, \"tags\": [\"a\", \"b\"]}";
    TB_CHECK_EQ(json_extract_string(body, "name"), std::string("alice"));
    TB_CHECK_EQ(json_extract_number(body, "balance"), std::string("1250"));
    const std::vector<std::string> tags = json_array_elements(body, "tags");
    TB_CHECK_EQ(tags.size(), static_cast<std::size_t>(2));
    TB_CHECK_EQ(json_extract_string(body, "missing"), std::string(""));
    return 0;
}

int test_user_service_records() {
    TB_CASE("user service record handling");
    Config config;
    Config::parse("user.default_role = viewer\n", config);
    UserService users(config);

    TB_CHECK(users.add_from_record("u1:Alice:ALICE@Example.COM:admin"));
    TB_CHECK(!users.add_from_record("broken-record"));
    TB_CHECK(!users.add_from_record("u1:Duplicate:x@y.z:admin"));
    TB_CHECK_EQ(users.user_count(), static_cast<std::size_t>(1));

    const User* u = users.find_user("u1");
    TB_CHECK(u != nullptr);
    if (u != nullptr) {
        TB_CHECK_EQ(u->email, std::string("alice@example.com"));
        TB_CHECK_EQ(u->role, std::string("admin"));
    }
    return 0;
}

int test_account_transfer_rules() {
    TB_CASE("account transfer rules");
    AccountService accounts;
    AuditLog audit;

    TB_CHECK(accounts.open_account("a1", "alice", Money::from_cents(10000)));
    TB_CHECK(accounts.open_account("a2", "bob", Money::from_cents(0)));
    TB_CHECK(!accounts.open_account("a1", "duplicate", Money::from_cents(0)));

    TB_CHECK(accounts.transfer("a1", "a2", Money::from_cents(2500), audit) ==
             TransferStatus::Ok);
    TB_CHECK(accounts.transfer("a1", "nope", Money::from_cents(1), audit) ==
             TransferStatus::UnknownTarget);
    TB_CHECK(accounts.transfer("a1", "a2", Money::from_cents(999999), audit) ==
             TransferStatus::InsufficientFunds);
    TB_CHECK(accounts.transfer("a1", "a2", Money::from_cents(0), audit) ==
             TransferStatus::InvalidAmount);

    TB_CHECK_EQ(audit.size(), static_cast<std::size_t>(1));
    TB_CHECK_EQ(accounts.total_balance().cents(), static_cast<std::int64_t>(10000));
    return 0;
}

int test_audit_chain() {
    TB_CASE("audit chain digest");
    AuditLog audit(4);
    for (int i = 0; i < 6; ++i) {
        audit.record("u1", "action", std::to_string(i));
    }
    TB_CHECK_EQ(audit.size(), static_cast<std::size_t>(4));
    TB_CHECK_EQ(audit.chain_digest().size(), static_cast<std::size_t>(64));

    const std::string before = audit.chain_digest();
    audit.clear();
    TB_CHECK(before != audit.chain_digest());
    return 0;
}

int test_ai_advisor_flow() {
    TB_CASE("ai advisor prompt and response handling");
    Config config;
    Config::parse("ai.model = demo-model\nai.endpoint = localhost\n", config);

    UserService users(config);
    users.add_from_record("u1:Alice:alice@example.com:admin");
    AccountService accounts;
    accounts.open_account("u1", "alice", Money::from_cents(5000));
    AuditLog audit;

    const std::string prompt = build_advice_prompt({"u1", "how much do I have?"}, users, accounts);
    TB_CHECK_MSG(prompt.find("Alice") != std::string::npos, "prompt includes the user name");
    TB_CHECK_MSG(prompt.find("how much do I have?") != std::string::npos,
                 "prompt includes the question");

    TB_CHECK_EQ(parse_model_response("{\"answer\":\"forty two\"}"), std::string("forty two"));
    TB_CHECK_EQ(parse_model_response("plain text"), std::string("plain text"));

    AdviceService advisor(config, users, accounts, audit);
    const AdviceResult r = advisor.advise({"u1", "balance?"});
    TB_CHECK(r.ok);
    TB_CHECK_EQ(r.model, std::string("demo-model"));
    TB_CHECK_EQ(r.request_fingerprint.size(), static_cast<std::size_t>(64));
    return 0;
}

int test_reconciliation_report() {
    TB_CASE("reconciliation report");
    AccountService accounts;
    AuditLog audit;
    accounts.open_account("a1", "alice", Money::from_cents(1000));
    accounts.open_account("a2", "bob", Money::from_cents(2000));
    audit.record("a1", "open", "initial");

    const ReconciliationReport report = build_reconciliation(accounts, audit);
    TB_CHECK_EQ(report.rows.size(), static_cast<std::size_t>(2));
    TB_CHECK_EQ(report.total, std::string("30.00"));
    TB_CHECK(verify_reconciliation(report, accounts));

    const std::string text = render_reconciliation(report);
    TB_CHECK_MSG(text.find("total: 30.00") != std::string::npos, "rendered total");
    return 0;
}

}  // namespace

int test_sha256_suite() {
    test_sha256_empty();
    test_sha256_abc();
    test_sha256_avalanche();
    return 0;
}

int test_domain_suite() {
    test_strings_basics();
    test_money_parsing();
    test_money_arithmetic();
    test_config_parsing();
    test_json_extraction();
    return 0;
}

int test_service_suite() {
    test_user_service_records();
    test_account_transfer_rules();
    test_audit_chain();
    return 0;
}

int test_integration_suite() {
    test_ai_advisor_flow();
    test_reconciliation_report();
    return 0;
}
