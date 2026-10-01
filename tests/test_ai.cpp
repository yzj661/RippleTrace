// ============================================================
//  RippleTrace - test_ai.cpp
//  Unit tests for the JSON reader and the AI grounding layer.
//
//  These matter more than most tests here: the whole claim of the AI layer
//  is "the model may not decide what is impacted". That claim is only worth
//  anything if it is enforced in code and verified. Everything below runs
//  offline - no API key, no network.
// ============================================================
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "ai_explain.h"
#include "graph.h"
#include "impact.h"
#include "json.h"
#include "parser.h"
#include "utf8.h"

using namespace rippletrace;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void checkEq(const std::string& actual, const std::string& expected,
             const std::string& what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s\n        expected: %s\n        actual  : %s\n",
                    what.c_str(), expected.c_str(), actual.c_str());
    }
}

void checkEqInt(long long actual, long long expected, const std::string& what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("  FAIL  %s (expected %lld, got %lld)\n", what.c_str(),
                    expected, actual);
    }
}

void section(const char* name) {
    std::printf("[ %s ]\n", name);
}

// ------------------------------------------------------------
//  A tiny C++ source used to build a real graph for grounding tests.
//  Layout:
//      entry() -> alpha() -> target()
//      entry() -> beta()  -> target()
//      unrelated()             (must never appear in the impact set)
// ------------------------------------------------------------
const char* kSource = R"CPP(
int target(int x) {
    if (x > 0) { return x + 1; }
    return 0;
}

int alpha(int x) {
    return target(x);
}

int beta(int x) {
    return target(x) * 2;
}

void unrelated() {
    for (int i = 0; i < 3; ++i) { }
}

void entry() {
    alpha(1);
    beta(2);
}
)CPP";

CallGraph buildTestGraph() {
    CallGraph g;
    HeuristicParser p;
    ParseResult pr;
    p.parseUnit("test.cpp", kSource, false, pr);
    g.addParse(pr);
    g.resolve();
    return g;
}

// ------------------------------------------------------------
//  JSON reader tests
// ------------------------------------------------------------
void testJson() {
    section("json reader");

    // --- basic values ---
    {
        json::Value v;
        std::string err;
        check(json::parse(R"({"a":1,"b":"x","c":true,"d":null})", v, err), "object parses");
        checkEqInt(static_cast<long long>(v["a"].asNumber()), 1, "number field");
        checkEq(v["b"].asString(), "x", "string field");
        check(v["c"].asBool(), "bool field");
        check(v["d"].isNull(), "null field");
    }

    // --- escaped quotes inside strings: the case regex extraction got wrong ---
    {
        json::Value v;
        std::string err;
        check(json::parse(R"({"s":"he said \"hi\" and \\ done"})", v, err),
              "escapes parse");
        checkEq(v["s"].asString(), "he said \"hi\" and \\ done", "escaped content");
    }

    // --- unicode escape ---
    {
        json::Value v;
        std::string err;
        check(json::parse(R"({"u":"\u4e2d\u6587"})", v, err), "unicode escape parses");
        checkEq(v["u"].asString(), "\xe4\xb8\xad\xe6\x96\x87", "unicode decoded to UTF-8");
    }

    // --- nesting and arrays ---
    {
        json::Value v;
        std::string err;
        const char* doc = R"({"choices":[{"message":{"content":"hello"}}],"n":[1,2,3]})";
        check(json::parse(doc, v, err), "nested doc parses");
        checkEq(v.path("choices.0.message.content").asString(), "hello",
                "dotted path lookup");
        checkEqInt(static_cast<long long>(v["n"].size()), 3, "array size");
        checkEqInt(static_cast<long long>(v.path("n.2").asNumber()), 3, "array index");
        check(v.path("n.9").isNull(), "out of range index is null");
        check(v.path("missing.deep.path").isNull(), "missing path is null");
    }

    // --- malformed input must fail, not silently succeed ---
    {
        json::Value v;
        std::string err;
        check(!json::parse(R"({"a":})", v, err), "trailing colon rejected");
        check(!json::parse(R"({"a":1)", v, err), "unterminated object rejected");
        check(!json::parse(R"({"a":1} extra)", v, err), "trailing data rejected");
        check(!json::parse("", v, err), "empty input rejected");
        check(!json::parse("not json at all", v, err), "prose rejected");
    }

    // --- invalid UTF-8 must be repaired, never emitted raw or as a lone
    //     surrogate. A model endpoint rejects such a body outright:
    //         HTTP 400 ... lone leading surrogate in hex escape
    //     The byte string here is a truncated 3-byte sequence followed by a
    //     stray continuation byte - exactly what byte-wise tokenising used to
    //     produce when it split a Chinese comment mid-character.
    {
        const std::string broken = std::string("ok") + "\xE4\xB8" + "\x80\x80" + "end";
        json::Value root = json::Value::makeObject();
        root.set("text", json::Value::makeString(broken));
        const std::string enc = json::serialize(root);

        // 1. the serialised form must itself be valid UTF-8
        bool valid = true;
        for (size_t i = 0; i < enc.size();) {
            unsigned char c = static_cast<unsigned char>(enc[i]);
            if (c < 0x80) { ++i; continue; }
            int len = 0;
            if (c >= 0xC2 && c <= 0xDF) len = 2;
            else if (c >= 0xE0 && c <= 0xEF) len = 3;
            else if (c >= 0xF0 && c <= 0xF4) len = 4;
            if (len == 0 || i + static_cast<size_t>(len) > enc.size()) { valid = false; break; }
            for (int k = 1; k < len; ++k) {
                unsigned char cc = static_cast<unsigned char>(enc[i + k]);
                if (cc < 0x80 || cc > 0xBF) { valid = false; break; }
            }
            if (!valid) break;
            i += static_cast<size_t>(len);
        }
        check(valid, "serialized JSON is valid UTF-8 even when the input was not");

        // 2. no half of a surrogate pair may appear
        check(enc.find("\\ud8") == std::string::npos &&
              enc.find("\\udc") == std::string::npos,
              "serializer emits no lone surrogate escape");

        // 3. it still round-trips
        json::Value back;
        std::string err;
        check(json::parse(enc, back, err), "repaired JSON re-parses");
        check(back["text"].asString().find("ok") == 0, "readable prefix preserved");
        check(back["text"].asString().find("end") != std::string::npos,
              "readable suffix preserved");
    }
    // --- round trip through the serializer ---
    {
        json::Value root = json::Value::makeObject();
        root.set("model", json::Value::makeString("m1"));
        root.set("temperature", json::Value::makeNumber(0.2));
        json::Value arr = json::Value::makeArray();
        json::Value m = json::Value::makeObject();
        m.set("role", json::Value::makeString("user"));
        m.set("content", json::Value::makeString("quote \" and \\ and \n newline"));
        arr.push(std::move(m));
        root.set("messages", std::move(arr));

        const std::string s = json::serialize(root);
        json::Value back;
        std::string err;
        check(json::parse(s, back, err), "serialized output re-parses");
        checkEq(back.path("messages.0.content").asString(),
                "quote \" and \\ and \n newline", "round trip preserves escapes");
        checkEqInt(static_cast<long long>(back["temperature"].asNumber() * 10), 2,
                   "round trip preserves number");
    }
}

// ------------------------------------------------------------
//  grounding enforcement - the core guarantee
// ------------------------------------------------------------
void testGrounding() {
    section("grounding enforcement");

    CallGraph g = buildTestGraph();
    ImpactAnalyzer analyzer(g);
    ImpactReport rep = analyzer.analyze({"target"});

    // sanity: the impact set is what we think it is
    checkEqInt(static_cast<long long>(rep.impacted.size()), 3,
               "target impacts alpha, beta, entry");
    check(g.has("unrelated"), "unrelated() exists in the graph");

    auto makeExplanation = [](std::vector<std::pair<std::string, int>> pts) {
        AiExplanation ex;
        ex.ok = true;
        for (const auto& p : pts) {
            ReviewPoint rp;
            rp.function = p.first;
            rp.priority = p.second;
            rp.what_to_check = "check " + p.first;
            ex.points.push_back(rp);
        }
        return ex;
    };

    // --- 1. a grounded point survives ---
    {
        AiExplanation ex = makeExplanation({{"alpha", 1}, {"entry", 2}});
        const int removed = enforceGrounding(g, rep, ex);
        checkEqInt(removed, 0, "grounded points are not removed");
        checkEqInt(static_cast<long long>(ex.points.size()), 2, "both grounded points kept");
    }

    // --- 2. THE KEY CASE: a function that exists but is NOT impacted ---
    {
        AiExplanation ex = makeExplanation({{"alpha", 1}, {"unrelated", 2}});
        const int removed = enforceGrounding(g, rep, ex);
        checkEqInt(removed, 1, "invented impact is rejected");
        checkEqInt(static_cast<long long>(ex.points.size()), 1, "only the grounded point remains");
        checkEq(ex.points[0].function, "alpha", "the surviving point is the correct one");
        checkEqInt(static_cast<long long>(ex.rejectedClaims.size()), 1,
                   "rejection is recorded, not hidden");
        const std::string& reason = ex.rejectedClaims[0];
        check(reason.find("unrelated") != std::string::npos, "rejection names the function");
        check(reason.find("NOT in the computed impact set") != std::string::npos,
              "rejection explains it was inferring impact");
    }

    // --- 3. a function that does not exist at all ---
    {
        AiExplanation ex = makeExplanation({{"totally_made_up", 1}});
        const int removed = enforceGrounding(g, rep, ex);
        checkEqInt(removed, 1, "hallucinated name is rejected");
        check(ex.rejectedClaims[0].find("no such function") != std::string::npos,
              "reason distinguishes hallucination from over-reach");
    }

    // --- 4. the changed function itself is an allowed target ---
    {
        AiExplanation ex = makeExplanation({{"target", 1}});
        checkEqInt(enforceGrounding(g, rep, ex), 0, "the changed function is allowed");
    }

    // --- 5. namespace-prefix tolerance must be unambiguous ---
    {
        AiExplanation ex = makeExplanation({{"alpha", 1}});
        checkEqInt(enforceGrounding(g, rep, ex), 0, "exact id accepted");
    }

    // --- 6. grounding is a FILTER: it must preserve the order it is given.
    //        (parseExplanation sorts by priority; re-sorting during filtering
    //         would override the model's intended emphasis.)
    {
        AiExplanation ex = makeExplanation({{"alpha", 1}, {"beta", 3}, {"entry", 5}});
        enforceGrounding(g, rep, ex);
        checkEqInt(ex.points[0].priority, 1, "order preserved: first point stays first");
        checkEq(ex.points[0].function, "alpha", "order preserved: alpha first");
        checkEq(ex.points[2].function, "entry", "order preserved: entry last");
    }

    // --- 7. and it still returns points in the order given after filtering ---
    {
        AiExplanation ex = makeExplanation({{"unrelated", 1}, {"beta", 2}, {"alpha", 3}});
        enforceGrounding(g, rep, ex);
        checkEqInt(static_cast<long long>(ex.points.size()), 2, "filtered to two points");
        checkEq(ex.points[0].function, "beta", "surviving order preserved");
        checkEq(ex.points[1].function, "alpha", "surviving order preserved (2)");
    }

    // --- 8. THE REGRESSION THAT MOTIVATED ALL OF THIS: the prompt prints each
    //        function as "id | file:line | ..." and models copy that whole label
    //        back. It names a real function, so it must be kept, not rejected.
    //        (Before the fix this rejected every point in 3 runs out of 4.)
    {
        std::string label;
        for (const auto& im : rep.impacted) {
            if (im.qualified == "alpha") {
                label = im.qualified + " | " + im.file + ":" +
                        std::to_string(im.line);
            }
        }
        check(!label.empty(), "alpha is in the impact set, so a label can be built");

        AiExplanation ex = makeExplanation({{label, 1}});
        checkEqInt(enforceGrounding(g, rep, ex), 0,
                   "a point quoting the full FACTS label is kept");
        checkEq(ex.points[0].function, "alpha",
                "the label is reduced to the bare id");

        // The same function named with a fuller path must still resolve.
        AiExplanation ex2 = makeExplanation({{"alpha | src/" + label, 1}});
        checkEqInt(enforceGrounding(g, rep, ex2), 0,
                   "a label with an extra path prefix is still kept");
    }

    // --- 9. decorative quoting a model may wrap the id in ---
    {
        AiExplanation ex = makeExplanation({{"`alpha`", 1}, {"\"beta\"", 2}});
        checkEqInt(enforceGrounding(g, rep, ex), 0, "backticks and quotes stripped");
        checkEq(ex.points[0].function, "alpha", "id recovered from backticks");
        checkEq(ex.points[1].function, "beta", "id recovered from quotes");
    }

    // --- 10. naming one function while quoting another one's location is a real
    //         grounding failure and must survive the label tolerance above.
    {
        AiExplanation ex = makeExplanation({{"alpha | elsewhere/other.cpp:1", 1}});
        checkEqInt(enforceGrounding(g, rep, ex), 1,
                   "a label whose location disagrees with the named function is rejected");
        check(!ex.rejectedClaims.empty() &&
                  ex.rejectedClaims[0].find("different function") != std::string::npos,
              "rejection explains the location mismatch");
    }

    // --- 11. an invented name is still rejected even when dressed as a label,
    //         and the reason must name the id rather than the whole label.
    {
        AiExplanation ex = makeExplanation({{"nosuch_function | test.cpp:1", 1}});
        checkEqInt(enforceGrounding(g, rep, ex), 1,
                   "an invented name in label form is rejected");
        check(!ex.rejectedClaims.empty() &&
                  ex.rejectedClaims[0].find("no such function") != std::string::npos,
              "still distinguishes hallucination from over-reach");
    }
}

// ------------------------------------------------------------
//  model response parsing
// ------------------------------------------------------------
void testResponseParsing() {
    section("model response parsing");

    // --- the expected shape ---
    {
        const char* reply = R"({
          "summary": "Wide blast radius.",
          "points": [
            {"function":"beta","priority":2,"what_to_check":"return value",
             "why_it_matters":"doubles the result"},
            {"function":"alpha","priority":1,"what_to_check":"null handling",
             "why_it_matters":"direct caller"}
          ]
        })";
        AiExplanation ex;
        std::string err;
        check(parseExplanation(reply, ex, err), "well formed reply parses");
        checkEq(ex.summary, "Wide blast radius.", "summary captured");
        checkEqInt(static_cast<long long>(ex.points.size()), 2, "both points captured");
        checkEq(ex.points[0].function, "alpha", "points sorted by priority");
        checkEq(ex.points[0].what_to_check, "null handling", "field captured");
    }

    // --- markdown fences: models add them despite instructions ---
    {
        const char* reply = "```json\n{\"summary\":\"s\",\"points\":[]}\n```";
        AiExplanation ex;
        std::string err;
        check(parseExplanation(reply, ex, err), "fenced reply is tolerated");
        checkEq(ex.summary, "s", "fenced content parsed");
    }

    // --- a point without a function is unusable and dropped ---
    {
        const char* reply = R"({"summary":"s","points":[{"priority":1,"what_to_check":"x"}]})";
        AiExplanation ex;
        std::string err;
        check(parseExplanation(reply, ex, err), "reply with an anon point still parses");
        checkEqInt(static_cast<long long>(ex.points.size()), 0, "anonymous point dropped");
    }

    // --- out of range priority is clamped ---
    {
        const char* reply = R"({"summary":"s","points":[{"function":"a","priority":99}]})";
        AiExplanation ex;
        std::string err;
        check(parseExplanation(reply, ex, err), "odd priority parses");
        checkEqInt(ex.points[0].priority, 5, "priority clamped to 5");
    }

    // --- broken replies fail loudly ---
    {
        AiExplanation ex;
        std::string err;
        check(!parseExplanation("I could not analyse this.", ex, err),
              "prose reply rejected");
        check(!err.empty(), "rejection carries an error message");
    }
}

// ------------------------------------------------------------
//  prompt construction
// ------------------------------------------------------------
void testPrompt() {
    section("prompt construction");

    CallGraph g = buildTestGraph();
    ImpactAnalyzer analyzer(g);
    ImpactReport rep = analyzer.analyze({"target"});

    AiOptions opt;
    const std::string sys = buildSystemPrompt();
    const std::string usr = buildExplanationPrompt(g, rep, opt);

    check(sys.find("Do NOT add") != std::string::npos,
          "system prompt forbids changing the impact set");
    check(sys.find("Never invent") != std::string::npos,
          "system prompt forbids inventing identifiers");
    check(sys.find("JSON") != std::string::npos, "system prompt demands JSON");

    check(usr.find("=== FACTS") != std::string::npos, "facts section present");
    check(usr.find("target") != std::string::npos, "changed function listed");
    check(usr.find("alpha") != std::string::npos, "impacted function listed");
    check(usr.find("beta") != std::string::npos, "second impacted function listed");
    check(usr.find("unrelated") == std::string::npos,
          "a non-impacted function is NOT put in the prompt");
    check(usr.find("alpha -> target") != std::string::npos ||
          usr.find("alpha -> target") != std::string::npos,
          "access path included as evidence");

    // determinism: the same report must produce the same prompt
    const std::string usr2 = buildExplanationPrompt(g, rep, opt);
    checkEq(usr2, usr, "prompt is deterministic");

    // ---- answer language ----
    // The default is Simplified Chinese: the UI, the report and the readers are
    // Chinese, so an English answer is a translation chore for the reader.
    AiOptions def;
    checkEq(def.language, std::string("zh"), "default answer language is zh");
    check(sys.find("简体中文") != std::string::npos,
          "default system prompt asks for Simplified Chinese");
    check(sys.find("never translate") != std::string::npos,
          "system prompt protects identifiers from translation");

    const std::string sysEn = buildSystemPrompt("en");
    check(sysEn.find("in English") != std::string::npos,
          "language=en asks for English");
    check(sysEn.find("简体中文") == std::string::npos,
          "language=en does not also ask for Chinese");

    const std::string sysAuto = buildSystemPrompt("auto");
    check(sysAuto.find("same language the user writes in") != std::string::npos,
          "language=auto follows the user");

    // An unrecognised code must not silently fall back to English.
    const std::string sysOdd = buildSystemPrompt("klingon");
    check(sysOdd.find("简体中文") != std::string::npos,
          "an unknown language code falls back to the default, not to English");

    // The prompt actually sent must carry the configured language, not just the
    // default: this is what the CLI and the server both rely on.
    AiOptions enOpt;
    enOpt.language = "en";
    const std::string usrEn = buildExplanationPrompt(g, rep, enOpt);
    check(usrEn.find("=== FACTS") != std::string::npos,
          "language does not disturb the grounded context");

    // ---- language code normalisation ----
    // Operators will write this by hand in rippletrace.env; it has to be forgiving.
    checkEq(normaliseLanguage("zh"), "zh", "zh accepted");
    checkEq(normaliseLanguage("zh-CN"), "zh", "zh-CN accepted");
    checkEq(normaliseLanguage("zh_Hans"), "zh", "zh_Hans accepted");
    checkEq(normaliseLanguage("Chinese (Simplified)"), "zh",
            "prose form accepted");
    checkEq(normaliseLanguage("  ZH  "), "zh", "case and spaces tolerated");
    checkEq(normaliseLanguage("cn"), "zh", "cn accepted");
    checkEq(normaliseLanguage("Chinese"), "zh", "Chinese accepted");
    checkEq(normaliseLanguage("en"), "en", "en accepted");
    checkEq(normaliseLanguage("en-US"), "en", "en-US accepted");
    checkEq(normaliseLanguage("English"), "en", "English accepted");
    checkEq(normaliseLanguage("auto"), "auto", "auto accepted");
    checkEq(normaliseLanguage(""), "zh", "empty keeps the default");
    checkEq(normaliseLanguage("klingon"), "zh",
            "a typo keeps the default instead of failing");
    // "enterprise" must not be read as "en".
    checkEq(normaliseLanguage("enterprise"), "zh",
            "an unknown code starting with 'en' is not treated as English");
}

// ------------------------------------------------------------
//  offline behaviour: no key = no invented answer
// ------------------------------------------------------------
void testOffline() {
    section("offline behaviour");

    CallGraph g = buildTestGraph();
    ImpactAnalyzer analyzer(g);
    ImpactReport rep = analyzer.analyze({"target"});

    AiOptions opt;
    opt.enabled = false;

    // No client: the pipeline must report unavailability, never fabricate.
    AiExplanation ex = explainImpact(g, rep, opt, nullptr);
    check(!ex.ok, "no client means not ok");
    check(!ex.usedAi, "usedAi stays false");
    checkEq(ex.provider, "offline", "provider reported as offline");
    check(ex.promptChars > 0, "prompt was still built and measured");
    check(ex.points.empty(), "no invented review points");
    check(ex.error.find("RIPPLETRACE_AI_KEY") != std::string::npos,
          "error tells the user how to enable it");
}

// ------------------------------------------------------------
//  response envelope extraction
// ------------------------------------------------------------
void testEnvelope() {
    section("api envelope extraction");

    std::string content, err;

    const char* ok = R"({"choices":[{"message":{"role":"assistant","content":"hi there"}}]})";
    check(OpenAiCompatibleClient::extractContent(ok, content, err),
          "openai-shaped envelope parsed");
    checkEq(content, "hi there", "content extracted");

    const char* apiErr = R"({"error":{"message":"invalid api key","type":"auth"}})";
    check(!OpenAiCompatibleClient::extractContent(apiErr, content, err),
          "api error envelope rejected");
    check(err.find("invalid api key") != std::string::npos,
          "api error message surfaced verbatim");

    const char* empty = R"({"choices":[]})";
    check(!OpenAiCompatibleClient::extractContent(empty, content, err),
          "empty choices rejected");

    // request body must be valid JSON containing both messages
    OpenAiCompatibleClient client("https://example.invalid/v1/chat/completions", "k", "m1");
    const std::string body = client.buildRequestBody("SYS", "USR \"quoted\"");
    json::Value parsed;
    std::string perr;
    check(json::parse(body, parsed, perr), "request body is valid JSON");
    checkEq(parsed["model"].asString(), "m1", "model in request body");
    checkEq(parsed.path("messages.0.content").asString(), "SYS", "system message present");
    checkEq(parsed.path("messages.1.content").asString(), "USR \"quoted\"",
            "user message survives escaping");
}

// ------------------------------------------------------------
//  rippletrace.env parsing
//
//  The problem report's core complaint was that the AI layer could only be
//  configured from the environment of whichever terminal happened to start
//  the server. A config file fixes that - but only if the parser is forgiving
//  about what people actually write in such a file, because a key that is
//  silently mis-parsed looks exactly like a key that was never supplied.
// ------------------------------------------------------------
void testEnvFile() {
    section("rippletrace.env parsing");

    const std::string path = "rippletrace_env_test.tmp";
    {
        std::ofstream out(path.c_str(), std::ios::binary);
        out << "# a comment\r\n";
        out << "\r\n";
        out << "   \r\n";
        out << "RIPPLETRACE_AI_KEY=sk-plain\r\n";
        out << "RIPPLETRACE_AI_MODEL = \"quoted-model\"  \r\n";
        out << "export RIPPLETRACE_AI_ENDPOINT='https://example.invalid/v1/chat/completions'\r\n";
        out << "this line has no equals sign\r\n";
        out << "RIPPLETRACE_AI_KEY=sk-second\r\n";
        out << "=novalue\r\n";
        out << "  # an indented comment\r\n";
        out << "RIPPLETRACE_AI_HELPER=\r\n";
    }

    std::vector<EnvFileEntry> entries;
    std::string err;
    check(parseEnvFile(path, entries, err), "config file parsed");
    std::remove(path.c_str());

    // Comments, blanks, and prose lines must not become settings.
    checkEqInt((long long)entries.size(), 5, "only real KEY=VALUE lines kept");

    checkEq(entries[0].key, "RIPPLETRACE_AI_KEY", "first key read");
    checkEq(entries[0].value, "sk-plain", "plain value intact");

    checkEq(entries[1].key, "RIPPLETRACE_AI_MODEL", "whitespace around key trimmed");
    checkEq(entries[1].value, "quoted-model", "double quotes stripped");

    checkEq(entries[2].key, "RIPPLETRACE_AI_ENDPOINT", "export prefix stripped");
    checkEq(entries[2].value, "https://example.invalid/v1/chat/completions",
            "single quotes stripped, URL intact");
    check(entries[2].value.find('\r') == std::string::npos,
          "CRLF did not leak into the value");

    // Both copies are kept, in order: "later wins" belongs to the loader, not
    // the parser, so that a test can still see what the file said.
    checkEq(entries[3].key, "RIPPLETRACE_AI_KEY", "duplicate key preserved");
    checkEq(entries[3].value, "sk-second", "duplicate value preserved");

    checkEq(entries[4].key, "RIPPLETRACE_AI_HELPER", "trailing empty value is a setting");
    checkEq(entries[4].value, "", "empty value stays empty");

    // A UTF-8 BOM is what Notepad and PowerShell's Out-File leave behind, and
    // it would otherwise glue itself onto the first key name.
    const std::string bomPath = "rippletrace_env_bom_test.tmp";
    {
        std::ofstream out(bomPath.c_str(), std::ios::binary);
        out << "\xEF\xBB\xBFRIPPLETRACE_AI_KEY=sk-bom\n";
    }
    std::vector<EnvFileEntry> bomEntries;
    check(parseEnvFile(bomPath, bomEntries, err), "BOM file parsed");
    std::remove(bomPath.c_str());
    checkEqInt((long long)bomEntries.size(), 1, "BOM line produced one setting");
    checkEq(bomEntries[0].key, "RIPPLETRACE_AI_KEY", "BOM stripped from the key name");
    checkEq(bomEntries[0].value, "sk-bom", "BOM did not corrupt the value");

    // An empty file is not an error - it just configures nothing.
    const std::string emptyPath = "rippletrace_env_empty_test.tmp";
    { std::ofstream out(emptyPath.c_str(), std::ios::binary); }
    std::vector<EnvFileEntry> emptyEntries;
    check(parseEnvFile(emptyPath, emptyEntries, err), "empty file is not an error");
    std::remove(emptyPath.c_str());
    checkEqInt((long long)emptyEntries.size(), 0, "empty file yields no settings");

    // A missing file must be distinguishable from an unreadable one, because
    // "no config file" is the normal case.
    std::vector<EnvFileEntry> missing;
    check(!parseEnvFile("definitely_not_here_rippletrace.env", missing, err),
          "missing file reported as missing");
    check(!err.empty(), "missing file sets an error message");

    // The loader must be self-consistent: enabled exactly when a source
    // supplied a key. This holds whether or not the machine running the tests
    // happens to have a key configured.
    AiOptions opts = loadAiOptionsFromEnv();
    const bool hasSource = opts.keySource != "none";
    check(opts.enabled == hasSource,
          "enabled agrees with the reported key source");
    check(opts.keySource == "none" || opts.keySource == "environment" ||
              opts.keySource == "file",
          "key source is one of the documented values");
    check(!opts.model.empty(), "a model name is always available");
    check(!opts.endpoint.empty(), "an endpoint is always available");

    // The candidate list is what the 503 message and the page point the
    // operator at, so it must stay non-empty and ordered with rippletrace.env first.
    std::vector<std::string> candidates = aiConfigFileCandidates();
    check(!candidates.empty(), "config file candidates exist");
    checkEq(candidates[0], "rippletrace.env", "rippletrace.env is looked for first");
}

// ------------------------------------------------------------
//  UTF-8 well-formedness.
//
//  These cases exist because a real Visual Studio project on a Chinese Windows
//  install contained a byte pair that CP936 decoded to an unpaired UTF-16
//  surrogate, which Windows then wrote back out as WTF-8 (ED A0 80). Both the
//  source reader and the JSON encoder accepted it -- their validators only
//  checked the continuation-byte ranges -- so it reached the model prompt and
//  the endpoint rejected the body:
//      HTTP 400 ... lone leading surrogate in hex escape
//
//  "Well formed" is not "valid": surrogates, overlong forms and code points
//  past U+10FFFF all satisfy the continuation-byte rule.
// ------------------------------------------------------------
void testUtf8() {
    section("utf8 well-formedness");

    const std::string twoByte = "\xC3\xA9";           // U+00E9
    const std::string threeB  = "\xE4\xB8\xAD";       // U+4E2D
    const std::string fourB   = "\xF0\x9F\x98\x80";   // U+1F600

    check(utf8::isWellFormed(""), "empty string is well-formed");
    check(utf8::isWellFormed("plain ASCII"), "ASCII is well-formed");
    check(utf8::isWellFormed(twoByte), "2-byte sequence is well-formed");
    check(utf8::isWellFormed(threeB), "3-byte sequence is well-formed");
    check(utf8::isWellFormed(fourB), "4-byte sequence is well-formed");
    check(utf8::isWellFormed("a" + threeB + "b"), "mixed ASCII and multibyte is well-formed");

    // Accepted by the old range-only check; all four classes must now fail.
    check(!utf8::isWellFormed("\xED\xA0\x80"), "encoded high surrogate is rejected");
    check(!utf8::isWellFormed("\xED\xBF\xBF"), "encoded low surrogate is rejected");
    check(!utf8::isWellFormed("x\xED\xA0\x80y"), "surrogate inside readable text is rejected");
    check(!utf8::isWellFormed("\xE0\x80\x80"), "overlong 3-byte form is rejected");
    check(!utf8::isWellFormed("\xF0\x80\x80\x80"), "overlong 4-byte form is rejected");
    check(!utf8::isWellFormed("\xF4\x90\x80\x80"), "code point past U+10FFFF is rejected");

    // Malformed shapes.
    check(!utf8::isWellFormed("\x80"), "lone continuation byte is rejected");
    check(!utf8::isWellFormed("\xC0\xAF"), "0xC0 lead byte is rejected");
    check(!utf8::isWellFormed("\xC1\xBF"), "0xC1 lead byte is rejected");
    check(!utf8::isWellFormed("\xF5\x80\x80\x80"), "0xF5 lead byte is rejected");
    check(!utf8::isWellFormed("\xFE\xFF"), "0xFE/0xFF are rejected");
    check(!utf8::isWellFormed("\xE4\xB8"), "truncated sequence is rejected");
    check(!utf8::isWellFormed(threeB + "\x80"), "stray continuation after a sequence is rejected");
}

void testUtf8Sanitize() {
    section("utf8 sanitising");

    const std::string fffd = "\xEF\xBF\xBD";   // U+FFFD

    checkEq(utf8::sanitize(""), "", "empty input stays empty");
    checkEq(utf8::sanitize("plain ASCII"), "plain ASCII", "ASCII passes through unchanged");
    checkEq(utf8::sanitize("a\xE4\xB8\xAD" "b"), "a\xE4\xB8\xAD" "b",
            "valid multibyte passes through unchanged");

    // One ill-formed subsequence becomes exactly one replacement character.
    checkEq(utf8::sanitize("\xED\xA0\x80"), fffd, "encoded surrogate becomes one U+FFFD");
    checkEq(utf8::sanitize("\xE0\x80\x80"), fffd, "overlong form becomes one U+FFFD");
    checkEq(utf8::sanitize("\xF4\x90\x80\x80"), fffd, "out-of-range code point becomes one U+FFFD");
    checkEq(utf8::sanitize("\x80"), fffd, "lone continuation becomes one U+FFFD");

    // Readable text on both sides survives, which is the whole point.
    checkEq(utf8::sanitize("keep\xED\xA0\x80keep"), "keep" + fffd + "keep",
            "readable prefix and suffix survive a surrogate");

    // The contract the JSON encoder relies on.
    const std::string hostile = "a\xED\xA0\x80" "b\xE0\x80\x80" "c\x80" "d";
    check(utf8::isWellFormed(utf8::sanitize(hostile)),
          "sanitised output is always well-formed");

    // End to end: a string carrying a surrogate must serialise into JSON that
    // contains no lone surrogate escape and re-parses.
    json::Value v = json::Value::makeObject();
    v.set("content", json::Value::makeString(hostile));
    const std::string encoded = json::serialize(v);
    check(encoded.find("\\ud") == std::string::npos &&
          encoded.find("\\uD") == std::string::npos,
          "JSON body contains no surrogate escape");
    check(encoded.find(fffd) != std::string::npos,
          "the surrogate was replaced with U+FFFD in the JSON body");
    check(utf8::isWellFormed(encoded), "serialised JSON is valid UTF-8");
    json::Value back;
    std::string perr;
    check(json::parse(encoded, back, perr), "repaired JSON re-parses");
    checkEq(back["content"].asString(), "a" + fffd + "b" + fffd + "c" + fffd + "d",
            "round trip preserves the repaired text");
}

}  // namespace

int main() {
    std::printf("=== RippleTrace AI layer tests ===\n\n");

    testJson();
    testUtf8();
    testUtf8Sanitize();
    testGrounding();
    testResponseParsing();
    testPrompt();
    testOffline();
    testEnvelope();
    testEnvFile();

    std::printf("\n----------------------------------------\n");
    std::printf("checks: %d   failures: %d\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
    } else {
        std::printf("TESTS FAILED\n");
    }
    return g_failures == 0 ? 0 : 1;
}
