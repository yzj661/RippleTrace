// ============================================================
//  RippleTrace - graph.h
//  Function-level call graph with name resolution.
// ============================================================
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "parser.h"

namespace rippletrace {

struct GraphStats {
    size_t functions = 0;
    size_t edges = 0;
    size_t resolvedEdges = 0;
    size_t unresolvedEdges = 0;
    size_t tests = 0;
    size_t entryPoints = 0;
    size_t publicApi = 0;
    size_t ambiguousNames = 0;   // short names shared by >1 definition

    // Per-language counts, keyed by display name. Reported separately because
    // resolution quality is not the same across languages, so blending them
    // into one total would overstate confidence.
    struct LangCount {
        size_t files = 0;
        size_t functions = 0;
    };
    std::map<std::string, LangCount> perLanguage;
};

class CallGraph {
public:
    // Merge a parsed translation unit into the graph.
    void addParse(const ParseResult& pr);

    // Resolve callee names to definitions using scope-aware rules.
    //
    // Cross-language resolution is OFF by default and that default is the
    // important part. Measured on a three-language sample, the short-name
    // fallback produced 26 of 220 edges (11.8%) that crossed language
    // boundaries - a C++ JSON helper resolving to a Java find(), a Java main()
    // resolving to C++ test suites. Every one of those was a false edge, and a
    // false edge corrupts the impact set. Real cross-language calls go through
    // FFI/JNI/sockets and are not visible as direct calls anyway, so refusing
    // them is both safer and more honest than guessing.
    void resolve(bool allowCrossLanguage = false);

    const RepoFunction* find(const std::string& qualified) const;
    bool has(const std::string& qualified) const;

    // Direct callers / callees.
    std::vector<std::string> callersOf(const std::string& qualified) const;
    std::vector<std::string> calleesOf(const std::string& qualified) const;
    std::vector<const CallEdge*> edgesFrom(const std::string& qualified) const;

    // Short name -> qualified names (used for diagnostics and lookup).
    std::vector<std::string> findByShortName(const std::string& shortName) const;

    // All defined functions, sorted by qualified id.
    std::vector<const RepoFunction*> allFunctions() const;

    // The language id ("cpp", "java", "python") of a function, derived from the
    // file it is defined in.
    std::string languageOf(const std::string& qualified) const;

    // Record that a translation unit of the named language was merged in.
    void noteLanguageFile(const std::string& displayName) {
        perLanguage_[displayName].files++;
    }

    const std::vector<CallEdge>& edges() const { return edges_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    GraphStats stats() const;

    size_t functionCount() const { return funcs_.size(); }

private:
    std::string resolveCallee(const CallEdge& e, bool allowCrossLanguage,
                             bool& rejectedCrossLanguage) const;

    std::map<std::string, RepoFunction> funcs_;                 // qualified -> function
    std::unordered_map<std::string, std::vector<std::string>> byShortName_;
    std::vector<CallEdge> pending_;                              // pre-resolution edges
    std::vector<CallEdge> edges_;                                // resolved edges only
    std::vector<std::string> warnings_;
    size_t unresolvedEdges_ = 0;
    size_t crossLanguageRejected_ = 0;
    std::map<std::string, GraphStats::LangCount> perLanguage_;
};

}  // namespace rippletrace
