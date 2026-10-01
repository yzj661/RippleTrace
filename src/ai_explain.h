// ============================================================
//  RippleTrace - ai_explain.h
//  The AI layer: turns a *verified* impact report into review guidance.
//
//  Division of labour, and why it matters:
//
//    static analysis  ->  WHICH functions are affected, HOW they connect,
//                         and the evidence (file:line call paths)
//    language model   ->  WHAT to look at in each of those places, in words
//
//  The model never decides the impact set. It receives it as immutable fact
//  and may only annotate it. Every function id it returns is checked against
//  the graph; an id that is not in the impacted set is rejected and reported
//  rather than shown. That is what keeps an LLM in this pipeline honest.
// ============================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "graph.h"
#include "impact.h"
#include "json.h"

namespace rippletrace {

// ------------------------------------------------------------
//  Result types
// ------------------------------------------------------------
struct ReviewPoint {
    std::string function;     // must reference a real, impacted function id
    int priority = 3;         // 1 = look first, 5 = optional
    std::string what_to_check;
    std::string why_it_matters;
};

struct AiExplanation {
    bool ok = false;
    std::string error;

    std::string summary;
    std::vector<ReviewPoint> points;

    // Grounding audit. Non-empty means the model produced output that was
    // discarded, which is itself worth showing the user.
    std::vector<std::string> rejectedClaims;
    bool usedAi = false;

    std::string provider;     // "openai-compatible" or "offline"
    std::string model;
    int promptChars = 0;
    int rejectedCount = 0;
};

// ------------------------------------------------------------
//  Transport seam: lets the pipeline be tested with no network.
// ------------------------------------------------------------
class IChatClient {
public:
    virtual ~IChatClient() = default;
    // Returns the assistant message content, or false with `error` set.
    virtual bool complete(const std::string& systemPrompt,
                         const std::string& userPrompt,
                         std::string& contentOut,
                         std::string& error) = 0;
};

// One KEY=VALUE line from an rippletrace.env file.
struct EnvFileEntry {
    std::string key;
    std::string value;
};

struct AiOptions {
    bool enabled = false;
    std::string endpoint;        // e.g. https://api.deepseek.com/v1/chat/completions
    std::string apiKey;
    std::string model = "deepseek-chat";
    // Where the settings actually came from. Surfaced in the startup banner and
    // in /api/health so a disabled AI layer is diagnosable without guessing.
    // keySource is "environment", "file" or "none"; configFile is the env file
    // that was read, if any. Neither field ever carries the key itself.
    std::string keySource = "none";
    std::string configFile;
    // Language for the three prose fields the model writes (summary,
    // what_to_check, why_it_matters). "zh" (the default) asks for Simplified
    // Chinese, "en" for English, "auto" for "match the user". Function ids,
    // file paths and code identifiers are never translated, in any mode.
    std::string language = "zh";
    int maxFactsPerFunction = 6;
    int maxImpactedInPrompt = 40;
    bool verbose = false;
    // Fallback transport: a command that reads a JSON request on stdin and
    // writes {"ok":true,"content":"..."} on stdout. Its argument (argv[1])
    // receives the path to the bundled helper script. Needed because some
    // Windows environments cannot use the native TLS stack at all.
    std::string helperCommand;   // e.g. "python"
    std::string helperScript;    // e.g. ".../tools/llm_transport.py"
    bool helperEnabled = true;
};

// ------------------------------------------------------------
//  Pure functions - each is unit-testable on its own
// ------------------------------------------------------------

// Build the grounded prompt. Deterministic: same report -> same prompt.
std::string buildExplanationPrompt(const CallGraph& graph,
                                  const ImpactReport& report,
                                  const AiOptions& options);

// System instruction: defines the contract the model must satisfy.
// `language` selects the language of the prose fields: "zh" (default,
// Simplified Chinese), "en", or "auto" (follow the user, default English).
std::string buildSystemPrompt(const std::string& language = "zh");

// Map a user-supplied language code ("zh-CN", "English", " auto " ...) to one
// of "zh" / "en" / "auto". Anything unrecognised yields "zh", the default, so
// a typo in an environment variable never costs the user their answer.
std::string normaliseLanguage(const std::string& raw);

// Parse the model's JSON reply into review points.
// Accepts either a bare object or one wrapped in markdown code fences.
bool parseExplanation(const std::string& modelReply,
                     AiExplanation& out,
                     std::string& error);

// Drop any point whose `function` is not in the impacted set (or the changed
// set). Returns how many were discarded, appending a reason for each.
int enforceGrounding(const CallGraph& graph,
                    const ImpactReport& report,
                    AiExplanation& explanation);

// The whole pipeline. `client` may be null, in which case only the prompt is
// produced and `usedAi` stays false (offline / no-key mode).
AiExplanation explainImpact(const CallGraph& graph,
                           const ImpactReport& report,
                           const AiOptions& options,
                           IChatClient* client);

// ------------------------------------------------------------
//  Real client: OpenAI-compatible /chat/completions over HTTPS.
// ------------------------------------------------------------
class OpenAiCompatibleClient : public IChatClient {
public:
    OpenAiCompatibleClient(std::string endpoint, std::string apiKey, std::string model);
    bool complete(const std::string& systemPrompt,
                 const std::string& userPrompt,
                 std::string& contentOut,
                 std::string& error) override;

    // Exposed for testing: builds the request body without sending it.
    std::string buildRequestBody(const std::string& systemPrompt,
                                const std::string& userPrompt) const;

    // Exposed for testing: extracts content from a response body.
    static bool extractContent(const std::string& responseBody,
                              std::string& contentOut,
                              std::string& error);

    void setHelper(std::string command, std::string script) {
        helperCommand_ = std::move(command);
        helperScript_ = std::move(script);
    }

private:
    // Runs the helper subprocess. Returns false when it is unavailable.
    bool completeViaHelper(const std::string& body,
                          std::string& contentOut,
                          std::string& error);

    std::string endpoint_;
    std::string apiKey_;
    std::string model_;
    std::string helperCommand_;
    std::string helperScript_;
};

// Configuration, in this order of precedence:
//   1. real environment variables (RIPPLETRACE_AI_KEY / _ENDPOINT / _MODEL / ...)
//   2. an rippletrace.env (or .env) file next to the working directory or the exe
// Absent key in both means AI stays disabled.
AiOptions loadAiOptionsFromEnv();

// Read a KEY=VALUE config file. Lines that are blank, start with '#', or carry
// no '=' are skipped, not treated as errors. A leading "export ", surrounding
// whitespace, CRLF line endings and matching single/double quotes are all
// tolerated. Returns false (and sets error) only when the file cannot be
// opened, so a caller can distinguish "no file" from "empty file".
bool parseEnvFile(const std::string& path,
                  std::vector<EnvFileEntry>& out,
                  std::string& error);

// Paths searched for the config file, in order. Exposed for the health
// endpoint and for tests: the page tells the operator exactly which file to
// create.
std::vector<std::string> aiConfigFileCandidates();

// Build a client with the helper transport wired in when one is available.
// Both the CLI and the web server use this so the transport chain is identical.
std::unique_ptr<IChatClient> makeChatClient(const AiOptions& options);

// ------------------------------------------------------------
//  Renderers
// ------------------------------------------------------------
std::string renderAiExplanationText(const AiExplanation& explanation, bool useColor);
void appendAiExplanationJson(json::Value& root, const AiExplanation& explanation);

}  // namespace rippletrace
