// ============================================================
//  RippleTrace - parser.h
//  Function extraction + call-site extraction, driven by a LanguageProfile.
//
//  IRepoParser is the abstraction seam. Two engines implement it:
//    BraceParser   - C, C++ and Java (brace-scoped)
//    IndentParser  - Python (indentation-scoped)
//  A tree-sitter or libclang frontend can implement the same interface later
//  to obtain exact overload resolution and template instantiation data.
// ============================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lang.h"
#include "tokenizer.h"

namespace rippletrace {

// ------------------------------------------------------------
//  A single reason backing one caller -> callee edge. Keeping the
//  evidence (file, line, kind) is what lets the report answer
//  "why is this affected?" with a traceable justification instead of
//  a plausible-sounding guess.
// ------------------------------------------------------------
struct CallEvidence {
    std::string file;
    int line = 0;
    std::string kind;   // "direct" | "member" | "constructor" | "virtual-suspect"
};

struct CallEdge {
    std::string caller;          // qualified id of the calling function
    std::string callee;          // name as written at the call site
    std::string calleeQualified;// resolved qualified id (may equal callee)
    CallEvidence evidence;
    bool resolved = false;       // true when callee matched a known definition
};

struct RepoFunction {
    std::string name;            // short name, e.g. "load_config"
    std::string qualified;       // id used in the graph, e.g. "Config::load_config"
    std::string file;            // repo-relative path
    int declLine = 0;
    int bodyLineStart = 0;
    int bodyLineEnd = 0;

    std::string signature;       // one-line, whitespace-collapsed
    std::string returnType;

    bool isDefinition = false;   // has a body
    bool isTest = false;         // name/file matches test heuristics
    bool isEntryPoint = false;   // main / registered entry
    bool isPublicApi = false;    // defined in a public header

    int cyclomatic = 1;          // 1 + branch points inside the body
    int lineCount = 0;
    int directCallCount = 0;     // out-degree (unique callees)
};

struct ParseResult {
    std::vector<RepoFunction> functions;
    std::vector<CallEdge> edges;
    std::vector<std::string> warnings;
};

class IRepoParser {
public:
    virtual ~IRepoParser() = default;
    // Parse one translation unit's text.
    virtual void parseUnit(const std::string& repoRelativePath,
                           const std::string& source,
                           bool isHeader,
                           ParseResult& out) = 0;
};

// ------------------------------------------------------------
//  Brace-scoped extractor: C, C++ and Java.
//
//  All three write callables as  ... name(params) { body }  and all three
//  resolve names through an enclosing scope whose separator is a profile
//  value ("::" for C++, "." for Java). The differences that matter are the
//  keyword tables and the scope separator, so one engine serves all three.
// ------------------------------------------------------------
class BraceParser : public IRepoParser {
public:
    explicit BraceParser(const LanguageProfile& lang) : lang_(&lang) {}
    void parseUnit(const std::string& repoRelativePath,
                   const std::string& source,
                   bool isHeader,
                   ParseResult& out) override;

private:
    const LanguageProfile* lang_;
};

// ------------------------------------------------------------
//  Indentation-scoped extractor: Python.
//
//  Python has no braces, so the block structure comes from the indentation
//  of lines. Functions are introduced by 'def' / 'async def'; a call site is
//  an identifier followed by '('.
// ------------------------------------------------------------
class IndentParser : public IRepoParser {
public:
    explicit IndentParser(const LanguageProfile& lang) : lang_(&lang) {}
    void parseUnit(const std::string& repoRelativePath,
                   const std::string& source,
                   bool isHeader,
                   ParseResult& out) override;

private:
    const LanguageProfile* lang_;
};

// Build the right parser for a profile. Returns nullptr for ScopeStyle values
// this build cannot handle, so callers can report it instead of guessing.
std::unique_ptr<IRepoParser> makeParser(const LanguageProfile& lang);

// The parser used before language profiles existed. Kept so the C++ path can
// be regression-tested against the refactor.
class HeuristicParser : public BraceParser {
public:
    HeuristicParser() : BraceParser(cppProfile()) {}
};

// Utility shared with the report layer.
std::string collapseWhitespace(const std::string& s);

}  // namespace rippletrace
