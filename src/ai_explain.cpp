// ============================================================
//  RippleTrace - ai_explain.cpp
// ============================================================
#include "ai_explain.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "report.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winhttp.h>
#  pragma comment(lib, "winhttp.lib")
#endif

namespace rippletrace {

namespace {

std::string trimCopy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Models often wrap JSON in ```json ... ``` despite being told not to.
std::string stripCodeFences(std::string s) {
    s = trimCopy(s);
    if (s.rfind("```", 0) == 0) {
        size_t nl = s.find('\n');
        if (nl != std::string::npos) s = s.substr(nl + 1);
        size_t end = s.rfind("```");
        if (end != std::string::npos) s = s.substr(0, end);
    }
    return trimCopy(s);
}

}  // namespace

// ------------------------------------------------------------
//  prompts
// ------------------------------------------------------------

namespace {

// The language the model writes the three prose fields in. Kept as one small
// function so "which languages do we support" stays greppable, and so the test
// suite can assert on it without matching a very large string.
std::string languageClause(const std::string& language) {
    if (language == "en" || language == "english") {
        return "Write summary, what_to_check and why_it_matters in English.";
    }
    if (language == "auto") {
        return "Write summary, what_to_check and why_it_matters in the same "
               "language the user writes in; default to concise technical "
               "English.";
    }
    // Default is Simplified Chinese. The UI, this report, and the engineers
    // reading it are Chinese, so an English answer is a translation chore for
    // the reader rather than extra information.
    return "Write summary, what_to_check and why_it_matters in Simplified "
           "Chinese (简体中文). A mixed-language answer is not acceptable.";
}

}  // namespace

std::string buildSystemPrompt(const std::string& language) {
    return
        "You are a senior C++ reviewer. You are given a machine-computed change "
        "impact report and you explain it to the engineer who made the change.\n"
        "\n"
        "Hard rules:\n"
        "1. The impact set is already computed and is authoritative. Do NOT add, "
        "remove or re-rank which functions are affected.\n"
        "2. Only reference function ids that appear in the FACTS section. Never "
        "invent a function name, file path or line number. The id is only the "
        "text before the first '|' on a FACTS line; do not copy the file, line, "
        "risk score or any other field into the function field.\n"
        "3. For each function you comment on, say concretely what to check there, "
        "based on the access path, the risk drivers and the line numbers given.\n"
        "4. If the facts are insufficient to say anything specific about a "
        "function, omit it rather than padding with generic advice.\n"
        "5. Answer with JSON only. No prose outside the JSON, no code fences.\n"
        "\n"
        "Required JSON shape:\n"
        "{\n"
        "  \"summary\": \"2-4 sentences on the overall blast radius and what is "
        "riskiest about this change\",\n"
        "  \"points\": [\n"
        "    {\n"
        "      \"function\": \"<exact id from FACTS>\",\n"
        "      \"priority\": 1,\n"
        "      \"what_to_check\": \"specific thing to verify in this function\",\n"
        "      \"why_it_matters\": \"why this function is on the path / what breaks\"\n"
        "    }\n"
        "  ]\n"
        "}\n"
        "priority is 1 (look first) to 5 (optional). Give at most 8 points, "
        "ordered by priority.\n"
        "\n"
        "6. " + languageClause(language) + " Function ids, file paths, code "
        "identifiers and API names stay exactly as they appear in the FACTS "
        "section - never translate those.";
}

std::string buildExplanationPrompt(const CallGraph& graph,
                                  const ImpactReport& report,
                                  const AiOptions& options) {
    std::ostringstream o;

    o << "=== CHANGE ===\n";
    for (const auto& c : report.changed) {
        const RepoFunction* f = graph.find(c);
        if (f) {
            o << "- " << c << "  (" << f->file << ":" << f->declLine
              << ", " << f->lineCount << " lines, cyclomatic " << f->cyclomatic
              << ", fan-in " << graph.callersOf(c).size() << ")\n";
        } else {
            o << "- " << c << "\n";
        }
    }
    if (!report.unknownChanged.empty()) {
        o << "- note: these requested names were not found in the graph: ";
        for (size_t i = 0; i < report.unknownChanged.size(); ++i) {
            if (i) o << ", ";
            o << report.unknownChanged[i];
        }
        o << "\n";
    }

    o << "\n=== BLAST RADIUS ===\n";
    o << "impacted functions: " << report.impacted.size()
      << " (max depth " << report.maxDepthReached << ")\n";
    o << "tests recommended: " << report.tests.size() << "\n";
    o << "change interactions: " << report.conflicts.size() << "\n";

    o << "\n=== FACTS: IMPACTED FUNCTIONS (authoritative, do not alter) ===\n";
    o << "Format: id | file:line | risk score | depth | fan-in | access path | "
         "risk drivers\n";
    o << "When quoting one of these in your answer, use only the id (the text "
         "before the first '|').\n";

    size_t emitted = 0;
    for (const auto& im : report.impacted) {
        if (emitted >= static_cast<size_t>(options.maxImpactedInPrompt)) {
            o << "... " << (report.impacted.size() - emitted)
              << " more impacted functions omitted from this prompt\n";
            break;
        }
        ++emitted;

        o << "- " << im.qualified
          << " | " << im.file << ":" << im.line
          << " | " << riskName(im.risk) << " " << im.riskScore
          << " | depth " << im.depth
          << " | fan-in " << im.fanIn
          << (im.isTest ? " | TEST" : "")
          << (im.isPublicApi ? " | PUBLIC-HEADER" : "")
          << "\n";

        if (!im.path.empty()) {
            o << "    path: ";
            for (size_t k = 0; k < im.path.size(); ++k) {
                if (k) o << " -> ";
                o << im.path[k];
            }
            o << "\n";
        }
        if (!im.reasons.empty()) {
            o << "    drivers: ";
            int shown = 0;
            for (const auto& r : im.reasons) {
                if (shown >= options.maxFactsPerFunction) break;
                if (shown) o << "; ";
                o << r;
                ++shown;
            }
            o << "\n";
        }
    }

    if (!report.tests.empty()) {
        o << "\n=== TESTS THAT REACH THE CHANGE ===\n";
        for (size_t i = 0; i < report.tests.size() && i < 20; ++i) {
            o << "- " << report.tests[i].qualified
              << " (" << report.tests[i].distance << " hop"
              << (report.tests[i].distance == 1 ? "" : "s") << ")\n";
        }
    }

    if (!report.conflicts.empty()) {
        o << "\n=== CHANGE INTERACTIONS ===\n";
        for (const auto& cp : report.conflicts) {
            o << "- " << cp.a << " <-> " << cp.b << ": " << cp.note
              << " (" << cp.sharedCallers << " shared caller(s))\n";
        }
    }

    if (!report.ambiguityNotes.empty()) {
        o << "\n=== LIMITATIONS OF THE STATIC ANALYSIS ===\n";
        for (const auto& n : report.ambiguityNotes) o << "- " << n << "\n";
        o << "Mention this uncertainty in the summary if it is material, but do "
             "not speculate about the unresolved call sites.\n";
    }

    o << "\n=== TASK ===\n";
    o << "Explain what the engineer should verify, per impacted function. "
         "Reference only the ids above. Return the JSON object described in the "
         "system message.\n";

    return o.str();
}

// ------------------------------------------------------------
//  response parsing
// ------------------------------------------------------------

bool parseExplanation(const std::string& modelReply,
                     AiExplanation& out,
                     std::string& error) {
    const std::string cleaned = stripCodeFences(modelReply);

    json::Value root;
    std::string perr;
    if (!json::parse(cleaned, root, perr)) {
        error = "model reply is not valid JSON (" + perr + ")";
        return false;
    }
    if (!root.isObject()) {
        error = "model reply is not a JSON object";
        return false;
    }

    out.summary = root["summary"].asString();

    const json::Value& pts = root["points"];
    if (pts.isArray()) {
        for (const auto& p : pts.items()) {
            if (!p.isObject()) continue;
            ReviewPoint rp;
            rp.function = p["function"].asString();
            if (rp.function.empty()) continue;   // unusable without a target

            double prio = p["priority"].asNumber(3);
            if (prio < 1) prio = 1;
            if (prio > 5) prio = 5;
            rp.priority = static_cast<int>(prio);

            rp.what_to_check = p["what_to_check"].asString();
            rp.why_it_matters = p["why_it_matters"].asString();
            out.points.push_back(std::move(rp));
        }
    }

    if (out.summary.empty() && out.points.empty()) {
        error = "model reply contained neither a summary nor any review point";
        return false;
    }

    std::stable_sort(out.points.begin(), out.points.end(),
                     [](const ReviewPoint& a, const ReviewPoint& b) {
                         return a.priority < b.priority;
                     });
    return true;
}

// ------------------------------------------------------------
//  grounding enforcement - the part that makes this trustworthy
//
//  Responsibility boundary: parseExplanation() owns ordering (it sorts by
//  priority); this function only filters. An earlier version re-sorted here,
//  which silently destroyed the priority order - a unit test caught it.
// ------------------------------------------------------------

// ------------------------------------------------------------
//  Model-supplied function references.
//
//  The FACTS section prints one line per function as
//      id | file:line | RISK n | depth d | fan-in f | ...
//  and models very often copy that whole label into the "function" field of the
//  reply. That is a correct identification of a real function, so looking the
//  compound string up as an id is a false rejection: on a multi-project
//  repository it rejected every point in three runs out of four, leaving the
//  user with an empty result and nothing but rejection notices.
//
//  So the id is read from the text before the first '|', decorative quoting is
//  stripped, and any location the model supplied is checked against the
//  function it named. Naming something outside the impact set, or naming one
//  function while locating it at another, is still rejected.
// ------------------------------------------------------------
struct ClaimedRef {
    std::string id;
    std::string file;
    int line = 0;
};

std::string lowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

std::string normaliseLanguage(const std::string& raw) {
    std::string lang = lowerAscii(trimCopy(raw));
    // Reduce to the primary subtag, so "zh-CN", "zh_Hans" and "en-US" work.
    // Matching on a raw prefix instead would read "enterprise" as English.
    const size_t cut = lang.find_first_of("-_ ");
    if (cut != std::string::npos) lang = lang.substr(0, cut);

    if (lang == "auto") return "auto";
    if (lang == "zh" || lang == "cn" || lang == "chinese") return "zh";
    if (lang == "en" || lang == "english") return "en";
    // Anything unrecognised keeps the default. A typo in an environment
    // variable must not cost the user their whole answer.
    return "zh";
}

std::string slashed(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(c == '\\' ? '/' : c);
    return out;
}

std::string baseName(const std::string& p) {
    const std::string s = slashed(p);
    const size_t slash = s.rfind('/');
    return lowerAscii(slash == std::string::npos ? s : s.substr(slash + 1));
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

ClaimedRef parseClaimedRef(const std::string& raw) {
    ClaimedRef ref;
    const size_t bar = raw.find('|');
    std::string head =
        trimCopy(bar == std::string::npos ? raw : raw.substr(0, bar));
    while (!head.empty() && (head.front() == '`' || head.front() == '"' ||
                             head.front() == '\'')) {
        head.erase(head.begin());
    }
    while (!head.empty() && (head.back() == '`' || head.back() == '"' ||
                             head.back() == '\'')) {
        head.pop_back();
    }
    ref.id = trimCopy(head);

    if (bar == std::string::npos) return ref;

    std::string rest = trimCopy(raw.substr(bar + 1));
    const size_t bar2 = rest.find('|');
    if (bar2 != std::string::npos) rest = trimCopy(rest.substr(0, bar2));

    const size_t colon = rest.rfind(':');
    if (colon == std::string::npos) return ref;

    const std::string filePart = trimCopy(rest.substr(0, colon));
    const std::string linePart = trimCopy(rest.substr(colon + 1));
    if (!filePart.empty() && allDigits(linePart)) {
        ref.file = filePart;
        ref.line = std::atoi(linePart.c_str());
    }
    return ref;
}

int enforceGrounding(const CallGraph& graph,
                    const ImpactReport& report,
                    AiExplanation& explanation) {
    std::unordered_set<std::string> allowed;
    // Where each allowed function lives, so a model that names one function but
    // quotes another one's location can still be caught.
    std::unordered_map<std::string, std::pair<std::string, int>> where;
    for (const auto& im : report.impacted) {
        allowed.insert(im.qualified);
        where[im.qualified] = std::make_pair(im.file, im.line);
    }
    for (const auto& c : report.changed) {
        allowed.insert(c);
        if (where.find(c) == where.end()) {
            const RepoFunction* f = graph.find(c);
            if (f) where[c] = std::make_pair(f->file, f->declLine);
        }
    }

    std::vector<ReviewPoint> kept;
    kept.reserve(explanation.points.size());

    for (auto& rp : explanation.points) {
        const std::string original = rp.function;
        const ClaimedRef ref = parseClaimedRef(rp.function);
        std::string id = ref.id;

        // Tolerate a model that drops the namespace prefix, but only when the
        // suffix resolves to exactly one function in the allowed set.
        if (!allowed.count(id)) {
            std::vector<std::string> matches;
            for (const auto& a : allowed) {
                if (a.size() > id.size() + 2 &&
                    a.compare(a.size() - id.size(), id.size(), id) == 0 &&
                    a[a.size() - id.size() - 1] == ':' &&
                    a[a.size() - id.size() - 2] == ':') {
                    matches.push_back(a);
                }
            }
            if (matches.size() == 1) {
                id = matches.front();
            }
        }

        if (!allowed.count(id)) {
            std::string reason = "rejected review point for '" + original + "'";
            if (graph.has(ref.id)) {
                reason += " - it exists in the repository but is NOT in the computed "
                          "impact set, so the model was inferring impact";
            } else {
                reason += " - no such function in the repository";
            }
            explanation.rejectedClaims.push_back(reason);
            continue;
        }

        // A supplied location has to agree with the function that was named.
        if (!ref.file.empty()) {
            auto it = where.find(id);
            if (it != where.end() && !it->second.first.empty()) {
                const bool fileOk = baseName(ref.file) == baseName(it->second.first);
                const bool lineOk = ref.line == 0 || it->second.second == 0 ||
                                    ref.line == it->second.second;
                if (!fileOk || !lineOk) {
                    explanation.rejectedClaims.push_back(
                        "rejected review point for '" + original + "' - it names " +
                        id + " but locates it at " + ref.file +
                        ", which is where a different function lives");
                    continue;
                }
            }
        }

        rp.function = id;
        kept.push_back(std::move(rp));
    }

    const int removed = static_cast<int>(explanation.points.size() - kept.size());
    explanation.points = std::move(kept);
    // Deliberately no re-sort: ordering by priority belongs to parseExplanation()
    // and re-sorting here would override the model's intended emphasis.
    explanation.rejectedCount = removed;
    return removed;
}

// ------------------------------------------------------------
//  pipeline
// ------------------------------------------------------------

// Build a client with the helper transport wired in when one is available.
// Used by both the CLI and the web server so the transports stay consistent.
std::unique_ptr<IChatClient> makeChatClient(const AiOptions& options) {
    auto client = std::unique_ptr<OpenAiCompatibleClient>(
        new OpenAiCompatibleClient(options.endpoint, options.apiKey, options.model));
    if (options.helperEnabled) {
        client->setHelper(options.helperCommand, options.helperScript);
    }
    return client;
}

AiExplanation explainImpact(const CallGraph& graph,
                           const ImpactReport& report,
                           const AiOptions& options,
                           IChatClient* client) {
    AiExplanation ex;

    const std::string systemPrompt = buildSystemPrompt(options.language);
    const std::string userPrompt = buildExplanationPrompt(graph, report, options);
    ex.promptChars = static_cast<int>(systemPrompt.size() + userPrompt.size());

    if (client == nullptr) {
        ex.ok = false;
        ex.usedAi = false;
        ex.provider = "offline";
        ex.error = "AI explanation is disabled (no client configured). "
                   "Set RIPPLETRACE_AI_KEY (and optionally RIPPLETRACE_AI_ENDPOINT / "
                   "RIPPLETRACE_AI_MODEL) to enable it.";
        return ex;
    }

    std::string content;
    std::string err;
    if (!client->complete(systemPrompt, userPrompt, content, err)) {
        ex.ok = false;
        ex.error = err.empty() ? "model request failed" : err;
        ex.provider = "openai-compatible";
        ex.model = options.model;
        return ex;
    }

    ex.usedAi = true;
    ex.provider = "openai-compatible";
    ex.model = options.model;

    if (!parseExplanation(content, ex, err)) {
        ex.ok = false;
        ex.error = err;
        return ex;
    }

    enforceGrounding(graph, report, ex);
    ex.ok = true;
    return ex;
}

// ------------------------------------------------------------
//  real HTTP client (WinHTTP)
// ------------------------------------------------------------

OpenAiCompatibleClient::OpenAiCompatibleClient(std::string endpoint,
                                              std::string apiKey,
                                              std::string model)
    : endpoint_(std::move(endpoint)), apiKey_(std::move(apiKey)), model_(std::move(model)) {}

std::string OpenAiCompatibleClient::buildRequestBody(const std::string& systemPrompt,
                                                    const std::string& userPrompt) const {
    json::Value root = json::Value::makeObject();
    root.set("model", json::Value::makeString(model_));
    root.set("temperature", json::Value::makeNumber(0.2));

    json::Value messages = json::Value::makeArray();
    {
        json::Value m = json::Value::makeObject();
        m.set("role", json::Value::makeString("system"));
        m.set("content", json::Value::makeString(systemPrompt));
        messages.push(std::move(m));
    }
    {
        json::Value m = json::Value::makeObject();
        m.set("role", json::Value::makeString("user"));
        m.set("content", json::Value::makeString(userPrompt));
        messages.push(std::move(m));
    }
    root.set("messages", std::move(messages));
    return json::serialize(root);
}

bool OpenAiCompatibleClient::extractContent(const std::string& responseBody,
                                           std::string& contentOut,
                                           std::string& error) {
    json::Value root;
    std::string perr;
    if (!json::parse(responseBody, root, perr)) {
        error = "cannot parse model response: " + perr;
        return false;
    }
    // Surface an API-level error message if present.
    if (root.has("error")) {
        const json::Value& e = root["error"];
        error = "API error: " + e["message"].asString(e["type"].asString("unknown error"));
        return false;
    }
    const json::Value& content = root.path("choices.0.message.content");
    if (!content.isString() || content.asString().empty()) {
        error = "model response contained no message content";
        return false;
    }
    contentOut = content.asString();
    return true;
}

// ------------------------------------------------------------
//  One HTTP exchange over WinHTTP.
//
//  `useProxy` selects between a direct connection and the system proxy. We
//  try direct first (correct for the common case and for localhost test
//  endpoints), then retry via the proxy, because a machine behind a
//  corporate proxy cannot reach the internet directly.
// ------------------------------------------------------------
namespace {

bool winHttpAttempt(const std::string& host,
                    INTERNET_PORT port,
                    const std::string& path,
                    bool https,
                    const std::string& apiKey,
                    const std::string& body,
                    bool useProxy,
                    DWORD& statusOut,
                    std::string& responseOut,
                    std::string& errorOut) {
    std::wstring whost(host.begin(), host.end());
    std::wstring wpath(path.begin(), path.end());

    HINTERNET session = WinHttpOpen(
        L"RippleTrace/0.1",
        useProxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        errorOut = "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    // Prefer IPv4. WinHttpConnect resolves the host itself and may try an
    // AAAA record first; on a machine with no IPv6 route the connection then
    // fails in a way that surfaces as an unhelpful 12185 rather than a
    // timeout. The failure is silent, so set this explicitly.
    //
    // WINHTTP_OPTION_IPV6 is not exported by the public winhttp.h in some SDK
    // versions, so define it here (value from the WinHTTP reference).
    {
#ifndef WINHTTP_OPTION_IPV6
#  define WINHTTP_OPTION_IPV6 1
#endif
        DWORD ipv6 = FALSE;
        WinHttpSetOption(session, WINHTTP_OPTION_IPV6, &ipv6, sizeof(ipv6));
    }

    HINTERNET connect = WinHttpConnect(session, whost.c_str(), port, 0);
    if (!connect) {
        errorOut = "cannot connect to " + host + ":" + std::to_string(port) +
                   " (error " + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        (https ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_BYPASS_PROXY_CACHE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        errorOut = "WinHttpOpenRequest failed";
        return false;
    }

    if (https) {
        // WinHTTP's default secure protocols depend on the OS build. On systems
        // where TLS 1.2 is not enabled by default, an https request fails with
        // 12185 (ERROR_WINHTTP_UNRECOGNIZED_SCHEME). Enabling modern TLS
        // explicitly is the documented fix.
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        if (!WinHttpSetOption(request, WINHTTP_OPTION_SECURE_PROTOCOLS,
                              &protocols, sizeof(protocols))) {
            // Not fatal: fall back to the platform default and let the request
            // decide. Recorded so a later failure is diagnosable.
            errorOut = "could not set TLS protocol mask (error " +
                       std::to_string(GetLastError()) + "); using platform default";
        } else {
            errorOut.clear();
        }
    }

    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!apiKey.empty()) {
        std::wstring key(apiKey.begin(), apiKey.end());
        headers += L"Authorization: Bearer " + key + L"\r\n";
    }

    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                            const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        const DWORD e = GetLastError();
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        errorOut = "WinHttpSendRequest failed (error " + std::to_string(e) + ")";
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        const DWORD e = GetLastError();
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        errorOut = "no response (error " + std::to_string(e) + ")";
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    std::string response;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, &chunk[0], avail, &read) || read == 0) break;
        chunk.resize(read);
        response += chunk;
        if (response.size() > 4u * 1024u * 1024u) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    statusOut = status;
    responseOut = std::move(response);
    return true;
}

}  // namespace

// ------------------------------------------------------------
//  Fallback transport: run a helper process and talk JSON over its stdio.
//
//  Needed because on this class of machine the native TLS stack is unusable
//  (Schannel: SEC_E_NO_CREDENTIALS), while an interpreter that bundles its own
//  TLS library still works. The API key travels through the child's stdin, not
//  through the command line, so it never appears in the process list.
// ------------------------------------------------------------
#if defined(_WIN32)
namespace {

std::string quoteWinArg(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

}  // namespace
#endif

bool OpenAiCompatibleClient::completeViaHelper(const std::string& body,
                                              std::string& contentOut,
                                              std::string& error) {
#if defined(_WIN32)
    if (helperCommand_.empty() || helperScript_.empty()) {
        error = "no helper transport configured";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE childStdinRead = nullptr, childStdinWrite = nullptr;
    HANDLE childStdoutRead = nullptr, childStdoutWrite = nullptr;

    if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)) {
        error = "helper: CreatePipe(stdin) failed";
        return false;
    }
    SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) {
        CloseHandle(childStdinRead); CloseHandle(childStdinWrite);
        error = "helper: CreatePipe(stdout) failed";
        return false;
    }
    SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdinRead;
    si.hStdOutput = childStdoutWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};

    // The helper receives the request as a single JSON object on stdin.
    json::Value request = json::Value::makeObject();
    request.set("url", json::Value::makeString(endpoint_));
    request.set("apiKey", json::Value::makeString(apiKey_));
    request.set("timeoutSec", json::Value::makeNumber(90));
    request.set("payload", json::parseOrNull(body));
    const std::string requestText = json::serialize(request);

    std::string cmd = quoteWinArg(helperCommand_) + " " + quoteWinArg(helperScript_);
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    BOOL created = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!created) {
        const DWORD e = GetLastError();
        CloseHandle(childStdinRead); CloseHandle(childStdinWrite);
        CloseHandle(childStdoutRead); CloseHandle(childStdoutWrite);
        error = "helper: CreateProcess failed (error " + std::to_string(e) + ")";
        return false;
    }

    // Parent must not hold the child's ends, or the read below never sees EOF.
    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);

    // ---- write the request ----
    {
        size_t written = 0;
        while (written < requestText.size()) {
            DWORD n = 0;
            if (!WriteFile(childStdinWrite, requestText.data() + written,
                           static_cast<DWORD>(requestText.size() - written), &n, nullptr)) {
                break;
            }
            written += n;
        }
    }
    CloseHandle(childStdinWrite);

    // ---- read the reply ----
    std::string reply;
    for (;;) {
        char buf[4096];
        DWORD n = 0;
        if (!ReadFile(childStdoutRead, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        reply.append(buf, n);
        if (reply.size() > 8u * 1024u * 1024u) break;
    }
    CloseHandle(childStdoutRead);

    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (reply.empty()) {
        error = "helper produced no output (exit code " + std::to_string(exitCode) +
                "); is '" + helperCommand_ + "' on PATH and is the helper script "
                "present at " + helperScript_ + "?";
        return false;
    }

    json::Value parsed;
    std::string perr;
    if (!json::parse(reply, parsed, perr)) {
        error = "helper reply is not valid JSON: " + perr;
        return false;
    }
    if (!parsed["ok"].asBool(false)) {
        error = parsed["error"].asString("helper reported an unspecified failure");
        return false;
    }
    contentOut = parsed["content"].asString();
    if (contentOut.empty()) {
        error = "helper returned an empty message";
        return false;
    }
    return true;
#else
    (void)body; (void)contentOut;
    error = "the helper transport is only implemented on Windows";
    return false;
#endif
}

bool OpenAiCompatibleClient::complete(const std::string& systemPrompt,
                                     const std::string& userPrompt,
                                     std::string& contentOut,
                                     std::string& error) {
#if defined(_WIN32)
    // ---- split endpoint into scheme / host / port / path ----
    // NOTE: WinHttpConnect() takes the host WITHOUT a port; the port must be
    // passed as a separate argument. An earlier version forwarded "host:port"
    // as the hostname, which made every request fail.
    std::string url = endpoint_;
    bool https = true;
    if (url.rfind("https://", 0) == 0) { url = url.substr(8); https = true; }
    else if (url.rfind("http://", 0) == 0) { url = url.substr(7); https = false; }
    else { error = "endpoint must start with http:// or https://"; return false; }

    std::string authority = url, path = "/";
    size_t slash = url.find('/');
    if (slash != std::string::npos) {
        authority = url.substr(0, slash);
        path = url.substr(slash);
    }

    std::string host = authority;
    INTERNET_PORT port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        std::string portText = authority.substr(colon + 1);
        if (!portText.empty() &&
            portText.find_first_not_of("0123456789") == std::string::npos) {
            host = authority.substr(0, colon);
            port = static_cast<INTERNET_PORT>(std::atoi(portText.c_str()));
        }
    }
    if (host.empty()) { error = "endpoint has no host"; return false; }

    const std::string body = buildRequestBody(systemPrompt, userPrompt);

    DWORD status = 0;
    std::string response, attemptError;

    // Attempt 1: direct. Attempt 2: through the system proxy.
    if (!winHttpAttempt(host, port, path, https, apiKey_, body, false,
                        status, response, attemptError)) {
        const std::string directError = attemptError;
        DWORD status2 = 0;
        std::string response2, proxyError;
        if (winHttpAttempt(host, port, path, https, apiKey_, body, true,
                           status2, response2, proxyError)) {
            status = status2;
            response = std::move(response2);
        } else {
            // Native TLS is unusable here. Try the helper transport before
            // giving up: on locked-down Windows images Schannel cannot acquire
            // credentials, while an interpreter with its own TLS library can.
            std::string helperError;
            const bool haveHelper = !helperCommand_.empty() && !helperScript_.empty();
            if (haveHelper && completeViaHelper(body, contentOut, helperError)) {
                return true;
            }
            std::string hint;
            if (directError.find("12185") != std::string::npos ||
                proxyError.find("12185") != std::string::npos) {
                hint = "\n  [12185 = ERROR_WINHTTP_UNRECOGNIZED_SCHEME. WinHTTP could "
                       "not set up https. On this machine the native TLS stack is "
                       "unavailable (Schannel reports SEC_E_NO_CREDENTIALS).]";
            }
            error = "cannot reach " + host + " directly (" + directError +
                    ") nor through the system proxy (" + proxyError + ")";
            if (haveHelper) {
                error += "\n  helper transport also failed: " + helperError;
            } else {
                error += "\n  no helper transport is configured";
            }
            error += hint;
            // A request with no key cannot succeed against a hosted endpoint,
            // but nothing in the transport errors above says so: the reader gets
            // TLS diagnostics and an HTTP 401 and has to guess. Anyone who runs
            // --ai before configuring a key would conclude the tool is broken.
            if (apiKey_.empty()) {
                error = "no API key is configured, so this request went out "
                        "unauthenticated. Set RIPPLETRACE_AI_KEY, add it to rippletrace.env, or "
                        "pass --ai-key (a keyless request only works against a local "
                        "endpoint that does not check credentials)\n  " + error;
            }
            return false;
        }
    }

    if (status < 200 || status >= 300) {
        std::string detail;
        json::Value r = json::parseOrNull(response);
        if (r.has("error")) detail = r["error"]["message"].asString();
        error = "model endpoint returned HTTP " + std::to_string(status);
        if (!detail.empty()) error += ": " + detail;
        if (apiKey_.empty()) {
            error += "\n  no API key is configured. Set RIPPLETRACE_AI_KEY, add it to "
                     "rippletrace.env, or pass --ai-key";
        }
        return false;
    }

    return extractContent(response, contentOut, error);
#else
    (void)systemPrompt; (void)userPrompt; (void)contentOut;
    error = "the AI layer currently uses WinHTTP and is only built on Windows";
    return false;
#endif
}

// ------------------------------------------------------------
//  configuration
// ------------------------------------------------------------

std::vector<std::string> aiConfigFileCandidates() {
    // rippletrace.env is the documented name; .env is also accepted because that is
    // what most developers already keep. Keep relative names first so the
    // normal project-root workflow remains predictable, then add paths next to
    // the executable so launching build\RippleTrace.exe from another directory also
    // finds the project's config file.
    std::vector<std::string> out = {"rippletrace.env", ".env", "../rippletrace.env", "../.env"};

#if defined(_WIN32)
    // GetModuleFileNameW returns the real executable path without passing it
    // through the process code page. That matters when the project lives under
    // a path containing Chinese characters.
    wchar_t buffer[32768]{};
    const DWORD n = GetModuleFileNameW(
        nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (n != 0 && n < static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]))) {
        const std::filesystem::path exePath(std::wstring(buffer, n));
        const std::filesystem::path exeDir = exePath.parent_path();
        const std::filesystem::path projectDir = exeDir.parent_path();
        const auto add = [&out](const std::filesystem::path& p) {
            const std::string candidate = p.u8string();
            if (std::find(out.begin(), out.end(), candidate) == out.end()) {
                out.push_back(candidate);
            }
        };
        add(exeDir / "rippletrace.env");
        add(exeDir / ".env");
        add(projectDir / "rippletrace.env");
        add(projectDir / ".env");
    }
#endif

    return out;
}

bool parseEnvFile(const std::string& path,
                  std::vector<EnvFileEntry>& out,
                  std::string& error) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        error = "cannot open config file: " + path;
        return false;
    }

    std::string line;
    bool firstLine = true;
    while (std::getline(in, line)) {
        if (firstLine) {
            firstLine = false;
            // Editors on Windows often leave a UTF-8 BOM behind.
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string s = trimCopy(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.rfind("export ", 0) == 0) s = trimCopy(s.substr(7));

        const size_t eq = s.find('=');
        // A line without '=' is prose, not a setting. Skipping it keeps a
        // hand-written file forgiving; it is not an error worth failing on.
        if (eq == std::string::npos) continue;

        std::string key = trimCopy(s.substr(0, eq));
        std::string val = trimCopy(s.substr(eq + 1));
        if (key.empty()) continue;

        if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') ||
                                (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        out.push_back(EnvFileEntry{key, val});
    }
    return true;
}

AiOptions loadAiOptionsFromEnv() {
    AiOptions o;
    auto getEnv = [](const char* name) -> std::string {
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string();
    };

    // ---- 1. optional config file ----
    // This is what makes the AI layer configurable without depending on the
    // environment of whichever terminal happened to start the server.
    std::map<std::string, std::string> fromFile;
    for (const auto& candidate : aiConfigFileCandidates()) {
        std::vector<EnvFileEntry> entries;
        std::string error;
        if (!parseEnvFile(candidate, entries, error)) continue;   // not present
        for (const auto& e : entries) fromFile[e.key] = e.value;  // later wins
        o.configFile = candidate;
        break;
    }

    // ---- 2. real environment variables override the file ----
    // Precedence this way round so an operator can override a checked-in
    // default for one run without editing or deleting anything.
    std::string origin;
    auto setting = [&](const char* name, std::string& whereFrom) -> std::string {
        whereFrom.clear();
        const std::string env = getEnv(name);
        if (!env.empty()) { whereFrom = "environment"; return env; }
        auto it = fromFile.find(name);
        if (it != fromFile.end() && !it->second.empty()) {
            whereFrom = "file";
            return it->second;
        }
        return std::string();
    };

    o.endpoint = setting("RIPPLETRACE_AI_ENDPOINT", origin);
    o.apiKey = setting("RIPPLETRACE_AI_KEY", o.keySource);
    std::string model = setting("RIPPLETRACE_AI_MODEL", origin);
    if (!model.empty()) o.model = model;

    // Language of the model's prose. Accepts "zh"/"zh-CN"/"chinese"/"cn",
    // "en"/"en-US"/"english", or "auto". Unrecognised values keep the default.
    const std::string lang = setting("RIPPLETRACE_AI_LANG", origin);
    if (!lang.empty()) o.language = normaliseLanguage(lang);

    if (o.endpoint.empty()) {
        // Default to a mainland-friendly OpenAI-compatible endpoint. Any
        // compatible service works; this is only a default.
        o.endpoint = "https://api.deepseek.com/v1/chat/completions";
    }
    if (o.keySource.empty()) o.keySource = "none";

    // Helper transport defaults: the bundled script next to the executable.
    // Overridable so a deployment can point at a different interpreter.
    o.helperCommand = setting("RIPPLETRACE_AI_HELPER", origin);
    if (o.helperCommand.empty()) o.helperCommand = "python";
    o.helperScript = setting("RIPPLETRACE_AI_HELPER_SCRIPT", origin);
    if (o.helperScript.empty()) {
        // Look for tools/llm_transport.py relative to the executable and to the
        // current directory, so both "run from project root" and "run the exe
        // from build/" work.
        const char* candidates[] = {
            "tools/llm_transport.py",
            "../tools/llm_transport.py",
            "../../tools/llm_transport.py",
        };
        for (const char* c : candidates) {
            FILE* f = std::fopen(c, "rb");
            if (f) { std::fclose(f); o.helperScript = c; break; }
        }
    }
    o.helperEnabled = !o.helperScript.empty();

    o.enabled = !o.apiKey.empty();
    return o;
}

// ------------------------------------------------------------
//  rendering
// ------------------------------------------------------------

std::string renderAiExplanationText(const AiExplanation& ex, bool useColor) {
    const char* BOLD = useColor ? "\x1b[1m" : "";
    const char* DIM = useColor ? "\x1b[2m" : "";
    const char* CYAN = useColor ? "\x1b[36m" : "";
    const char* YELLOW = useColor ? "\x1b[33m" : "";
    const char* RESET = useColor ? "\x1b[0m" : "";

    std::ostringstream o;
    o << "\n" << BOLD << "AI review guidance" << RESET << "\n";
    o << std::string(72, '-') << "\n";

    if (!ex.ok) {
        o << YELLOW << "  not available: " << ex.error << RESET << "\n";
        if (ex.promptChars > 0) {
            o << DIM << "  (a grounded prompt of " << ex.promptChars
              << " chars was prepared; no model was called)" << RESET << "\n";
        }
        return o.str();
    }

    o << DIM << "  provider=" << ex.provider << " model=" << ex.model
      << " prompt=" << ex.promptChars << " chars" << RESET << "\n";
    o << DIM << "  grounding: impact set is authoritative; "
      << ex.points.size() << " point(s) kept";
    if (ex.rejectedCount > 0) o << ", " << ex.rejectedCount << " rejected";
    o << RESET << "\n";

    if (!ex.summary.empty()) {
        o << "\n" << BOLD << "Summary" << RESET << "\n  " << ex.summary << "\n";
    }

    if (!ex.points.empty()) {
        o << "\n" << BOLD << "What to check, per impacted function" << RESET << "\n";
        for (const auto& p : ex.points) {
            o << "  " << CYAN << "[P" << p.priority << "]" << RESET << " "
              << BOLD << p.function << RESET << "\n";
            if (!p.what_to_check.empty())
                o << "       check : " << p.what_to_check << "\n";
            if (!p.why_it_matters.empty())
                o << "       why   : " << p.why_it_matters << "\n";
        }
    }

    if (!ex.rejectedClaims.empty()) {
        o << "\n" << YELLOW << BOLD
          << "Rejected model claims (not grounded in the computed impact set)"
          << RESET << "\n";
        for (const auto& r : ex.rejectedClaims) {
            o << "  ! " << r << "\n";
        }
        o << DIM
          << "  These were discarded rather than shown, because the model may not "
             "decide what is impacted." << RESET << "\n";
    }

    return o.str();
}

void appendAiExplanationJson(json::Value& root, const AiExplanation& ex) {
    json::Value ai = json::Value::makeObject();
    ai.set("requested", json::Value::makeBool(true));
    ai.set("usedAi", json::Value::makeBool(ex.usedAi));
    ai.set("ok", json::Value::makeBool(ex.ok));
    ai.set("provider", json::Value::makeString(ex.provider));
    ai.set("model", json::Value::makeString(ex.model));
    ai.set("promptChars", json::Value::makeNumber(ex.promptChars));
    ai.set("rejectedCount", json::Value::makeNumber(ex.rejectedCount));
    if (!ex.error.empty()) ai.set("error", json::Value::makeString(ex.error));

    ai.set("groundingRule",
           json::Value::makeString(
               "the impact set is computed by static analysis and is authoritative; "
               "the model may only annotate functions already in it"));

    if (!ex.summary.empty()) ai.set("summary", json::Value::makeString(ex.summary));

    json::Value pts = json::Value::makeArray();
    for (const auto& p : ex.points) {
        json::Value v = json::Value::makeObject();
        v.set("function", json::Value::makeString(p.function));
        v.set("priority", json::Value::makeNumber(p.priority));
        v.set("whatToCheck", json::Value::makeString(p.what_to_check));
        v.set("whyItMatters", json::Value::makeString(p.why_it_matters));
        pts.push(std::move(v));
    }
    ai.set("points", std::move(pts));

    json::Value rej = json::Value::makeArray();
    for (const auto& r : ex.rejectedClaims) {
        rej.push(json::Value::makeString(r));
    }
    ai.set("rejectedClaims", std::move(rej));

    root.set("ai", std::move(ai));
}

}  // namespace rippletrace
