// ============================================================
//  RippleTrace - impact.h
//  Blast-radius computation over the call graph.
//
//  This module produces the part of the report no prompt-only tool can
//  produce reliably: a *justified* reverse-reachability set. Every
//  impacted function carries the concrete call path that reaches it,
//  so the claim "X is affected" is always backed by file:line evidence.
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "graph.h"

namespace rippletrace {

enum class Risk { Low = 0, Medium = 1, High = 2, Critical = 3 };

const char* riskName(Risk r);

struct ImpactedFunction {
    std::string qualified;
    std::string file;
    int line = 0;
    int depth = 0;                    // 1 = direct caller of a changed function
    int fanIn = 0;                    // number of direct callers
    Risk risk = Risk::Low;
    int riskScore = 0;                // 0..100, for sorting
    bool isTest = false;
    bool isPublicApi = false;
    bool isEntryPoint = false;
    std::vector<std::string> reasons; // human-readable risk drivers
    // Shortest justification path from this function down to a changed one:
    // path[0] == this function, path.back() == a changed function.
    std::vector<std::string> path;
};

struct TestRecommendation {
    std::string qualified;
    std::string file;
    int distance = 0;                 // fewest hops to any changed function
    std::vector<std::string> path;
};

struct ConflictPair {
    std::string a;
    std::string b;
    int sharedCallers = 0;
    std::vector<std::string> examples;  // up to a few shared callers
    std::string note;
};

struct ImpactReport {
    std::vector<std::string> changed;        // resolved changed functions
    std::vector<std::string> unknownChanged; // names that could not be resolved
    std::vector<ImpactedFunction> impacted;  // sorted by risk desc, then depth asc
    std::vector<TestRecommendation> tests;   // sorted by distance asc
    std::vector<ConflictPair> conflicts;     // interaction between changes
    std::vector<std::string> ambiguityNotes; // unresolved-callee caveats
    int maxDepthReached = 0;
    size_t resolvedCalleeCount = 0;
    size_t unresolvedCalleeCount = 0;
};

struct ImpactOptions {
    int maxDepth = 6;
    bool includeTests = true;
    bool computeConflicts = true;
};

class ImpactAnalyzer {
public:
    explicit ImpactAnalyzer(const CallGraph& g) : g_(g) {}

    ImpactReport analyze(const std::vector<std::string>& changedNames,
                         const ImpactOptions& opt = ImpactOptions()) const;

    // Resolve a user-supplied name (short or qualified) to a graph id.
    // Returns empty when unknown; when several candidates share the short
    // name the caller gets all of them so ambiguity is visible, not hidden.
    std::vector<std::string> resolveName(const std::string& name) const;

private:
    int scoreRisk(const RepoFunction& f, int depth, int fanIn,
                  bool reachableFromTest, std::vector<std::string>& reasons) const;

    const CallGraph& g_;
};

}  // namespace rippletrace
