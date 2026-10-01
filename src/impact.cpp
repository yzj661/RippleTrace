// ============================================================
//  RippleTrace - impact.cpp
// ============================================================
#include "impact.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace rippletrace {

const char* riskName(Risk r) {
    switch (r) {
        case Risk::Critical: return "CRITICAL";
        case Risk::High:     return "HIGH";
        case Risk::Medium:   return "MEDIUM";
        default:             return "LOW";
    }
}

std::vector<std::string> ImpactAnalyzer::resolveName(const std::string& name) const {
    std::vector<std::string> out;
    if (name.empty()) return out;
    if (g_.has(name)) { out.push_back(name); return out; }

    // exact short-name match(es)
    std::vector<std::string> byShort = g_.findByShortName(name);
    if (!byShort.empty()) {
        std::sort(byShort.begin(), byShort.end());
        return byShort;
    }
    // suffix match: "foo" should also find "ns::foo" style ids already covered
    // above; here we additionally accept "Class::foo" given just "Class::foo"
    // with a leading namespace stripped.
    for (const RepoFunction* f : g_.allFunctions()) {
        if (f->qualified.size() > name.size() + 2 &&
            f->qualified.compare(f->qualified.size() - name.size(), name.size(), name) == 0 &&
            f->qualified[f->qualified.size() - name.size() - 1] == ':' &&
            f->qualified[f->qualified.size() - name.size() - 2] == ':') {
            out.push_back(f->qualified);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int ImpactAnalyzer::scoreRisk(const RepoFunction& f, int depth, int fanIn,
                              bool reachableFromTest,
                              std::vector<std::string>& reasons) const {
    int score = 0;
    reasons.clear();

    // --- fan-in: how many places must be re-checked ---
    if (fanIn >= 8)      { score += 30; reasons.push_back("high fan-in (" + std::to_string(fanIn) + " direct callers)"); }
    else if (fanIn >= 4) { score += 22; reasons.push_back("moderate fan-in (" + std::to_string(fanIn) + " direct callers)"); }
    else if (fanIn >= 2) { score += 12; reasons.push_back("shared by " + std::to_string(fanIn) + " callers"); }
    else if (fanIn == 1) { score += 4; }

    // --- distance: direct callers are hit hardest ---
    if (depth == 1)      { score += 28; reasons.push_back("direct caller of a changed function"); }
    else if (depth == 2) { score += 16; reasons.push_back("2 hops from a change"); }
    else if (depth == 3) { score += 8;  reasons.push_back("3 hops from a change"); }
    else                 { score += 3; }

    // --- public surface ---
    if (f.isPublicApi)   { score += 20; reasons.push_back("defined in a public header (externally visible)"); }

    // --- entry points are the riskiest to break silently ---
    if (f.isEntryPoint)  { score += 12; reasons.push_back("entry point"); }

    // --- low-level code amplifies blast radius ---
    if (f.lineCount <= 15 && f.cyclomatic <= 3 && fanIn >= 3) {
        score += 8;
        reasons.push_back("small utility with wide reuse");
    }

    // --- test coverage reduces uncertainty, so it reduces risk ---
    if (!reachableFromTest) {
        score += 14;
        reasons.push_back("no test path reaches this change");
    } else {
        reasons.push_back("covered by at least one test path");
    }

    if (score > 100) score = 100;
    return score;
}

ImpactReport ImpactAnalyzer::analyze(const std::vector<std::string>& changedNames,
                                     const ImpactOptions& opt) const {
    ImpactReport rep;

    // ---- 1. resolve changed names ----
    std::unordered_set<std::string> changed;
    for (const auto& n : changedNames) {
        auto r = resolveName(n);
        if (r.empty()) {
            rep.unknownChanged.push_back(n);
        } else {
            for (const auto& q : r) { changed.insert(q); rep.changed.push_back(q); }
        }
    }
    std::sort(rep.changed.begin(), rep.changed.end());
    rep.changed.erase(std::unique(rep.changed.begin(), rep.changed.end()), rep.changed.end());
    if (rep.changed.empty()) return rep;

    // ---- 2. reverse BFS giving each node a shortest path to a change ----
    // parent[v] = the node v calls that leads toward a changed function.
    std::unordered_map<std::string, std::string> parent;
    std::unordered_map<std::string, int> dist;
    std::deque<std::string> queue;

    for (const auto& c : rep.changed) {
        dist[c] = 0;
        queue.push_back(c);
    }

    while (!queue.empty()) {
        std::string cur = queue.front();
        queue.pop_front();
        int d = dist[cur];
        if (d >= opt.maxDepth) continue;

        for (const std::string& caller : g_.callersOf(cur)) {
            if (dist.count(caller)) continue;
            dist[caller] = d + 1;
            parent[caller] = cur;
            queue.push_back(caller);
        }
    }

    // ---- 3. which functions are reachable from a test (downward) ----
    // Computed once from every test entry, so risk scoring can tell whether a
    // change is protected by tests at all.
    std::unordered_set<std::string> testReachable;
    {
        std::deque<std::string> q;
        for (const RepoFunction* f : g_.allFunctions()) {
            if (f->isTest) { testReachable.insert(f->qualified); q.push_back(f->qualified); }
        }
        while (!q.empty()) {
            std::string cur = q.front();
            q.pop_front();
            for (const std::string& callee : g_.calleesOf(cur)) {
                if (testReachable.insert(callee).second) q.push_back(callee);
            }
        }
    }

    // ---- 4. build impacted list ----
    std::unordered_set<std::string> changedSet(rep.changed.begin(), rep.changed.end());
    for (const auto& kv : dist) {
        const std::string& id = kv.first;
        if (kv.second == 0) continue;               // the changes themselves
        const RepoFunction* f = g_.find(id);
        if (!f) continue;
        if (f->isTest && !opt.includeTests) continue;

        ImpactedFunction im;
        im.qualified = id;
        im.file = f->file;
        im.line = f->declLine;
        im.depth = kv.second;
        im.isTest = f->isTest;
        im.isPublicApi = f->isPublicApi;
        im.isEntryPoint = f->isEntryPoint;
        im.fanIn = static_cast<int>(g_.callersOf(id).size());

        // reconstruct the shortest justification path down to a change
        std::string cur = id;
        im.path.push_back(cur);
        while (parent.count(cur)) {
            cur = parent.at(cur);
            im.path.push_back(cur);
            if (changedSet.count(cur)) break;
        }

        bool reachable = testReachable.count(id) > 0;
        im.riskScore = scoreRisk(*f, im.depth, im.fanIn, reachable, im.reasons);
        if (im.riskScore >= 70)      im.risk = Risk::Critical;
        else if (im.riskScore >= 50) im.risk = Risk::High;
        else if (im.riskScore >= 30) im.risk = Risk::Medium;
        else                         im.risk = Risk::Low;

        rep.impacted.push_back(std::move(im));
        rep.maxDepthReached = std::max(rep.maxDepthReached, kv.second);
    }

    std::sort(rep.impacted.begin(), rep.impacted.end(),
              [](const ImpactedFunction& a, const ImpactedFunction& b) {
                  if (a.riskScore != b.riskScore) return a.riskScore > b.riskScore;
                  if (a.depth != b.depth) return a.depth < b.depth;
                  return a.qualified < b.qualified;
              });

    // ---- 5. test selection ----
    for (const auto& im : rep.impacted) {
        if (!im.isTest) continue;
        TestRecommendation tr;
        tr.qualified = im.qualified;
        tr.file = im.file;
        tr.distance = im.depth;
        tr.path = im.path;
        rep.tests.push_back(std::move(tr));
    }
    // Also: tests that call a *changed* function directly have depth 0 and are
    // excluded above, so pick them up explicitly.
    for (const auto& c : rep.changed) {
        const RepoFunction* f = g_.find(c);
        if (f && f->isTest) {
            TestRecommendation tr;
            tr.qualified = c;
            tr.file = f->file;
            tr.distance = 0;
            tr.path = {c};
            rep.tests.push_back(std::move(tr));
        }
    }
    std::sort(rep.tests.begin(), rep.tests.end(),
              [](const TestRecommendation& a, const TestRecommendation& b) {
                  if (a.distance != b.distance) return a.distance < b.distance;
                  return a.qualified < b.qualified;
              });
    rep.tests.erase(std::unique(rep.tests.begin(), rep.tests.end(),
                                [](const TestRecommendation& a, const TestRecommendation& b) {
                                    return a.qualified == b.qualified;
                                }),
                    rep.tests.end());

    // ---- 6. change-interaction conflicts ----
    // Two changed functions interacting means some caller depends on both:
    // the changes cannot be validated independently.
    if (opt.computeConflicts && rep.changed.size() >= 2) {
        std::unordered_map<std::string, std::unordered_set<std::string>> callersOfChange;
        for (const auto& c : rep.changed) {
            auto cs = g_.callersOf(c);
            callersOfChange[c] = std::unordered_set<std::string>(cs.begin(), cs.end());
        }
        for (size_t i = 0; i < rep.changed.size(); ++i) {
            for (size_t j = i + 1; j < rep.changed.size(); ++j) {
                const std::string& a = rep.changed[i];
                const std::string& b = rep.changed[j];
                std::vector<std::string> shared;
                for (const auto& ca : callersOfChange[a]) {
                    if (callersOfChange[b].count(ca)) shared.push_back(ca);
                }
                // also treat "a calls b" as an interaction
                bool direct = false;
                for (const auto& e : g_.edges()) {
                    if ((e.caller == a && e.calleeQualified == b) ||
                        (e.caller == b && e.calleeQualified == a)) { direct = true; break; }
                }
                if (shared.empty() && !direct) continue;

                ConflictPair cp;
                cp.a = a;
                cp.b = b;
                cp.sharedCallers = static_cast<int>(shared.size());
                std::sort(shared.begin(), shared.end());
                for (size_t k = 0; k < shared.size() && k < 5; ++k) cp.examples.push_back(shared[k]);
                if (direct) {
                    cp.note = "one changed function calls the other (direct dependency)";
                } else {
                    cp.note = "shared caller depends on both changes";
                }
                rep.conflicts.push_back(std::move(cp));
            }
        }
        std::sort(rep.conflicts.begin(), rep.conflicts.end(),
                  [](const ConflictPair& x, const ConflictPair& y) {
                      return x.sharedCallers > y.sharedCallers;
                  });
    }

    // ---- 7. ambiguity caveats ----
    // Unresolved call sites mean the impact set can only be a lower bound:
    // we say so explicitly rather than pretending the graph is complete.
    {
        rep.resolvedCalleeCount = g_.edges().size();
        GraphStats st = g_.stats();
        rep.unresolvedCalleeCount = st.unresolvedEdges;
        if (st.unresolvedEdges > 0) {
            rep.ambiguityNotes.push_back(
                std::to_string(st.unresolvedEdges) +
                " call site(s) could not be resolved to a definition in this repo "
                "(external library, template, or ambiguous overload); those edges are "
                "excluded from the impact set, so the result is a lower bound.");
        }
        if (st.ambiguousNames > 0) {
            rep.ambiguityNotes.push_back(
                std::to_string(st.ambiguousNames) +
                " short name(s) map to multiple definitions; such calls are only "
                "resolved when the scope chain or a unique global match decides it.");
        }
    }

    return rep;
}

}  // namespace rippletrace
