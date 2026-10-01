// ============================================================
//  RippleTrace - report.h
//  Rendering: console report, JSON, Graphviz DOT, test run-list.
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "graph.h"
#include "impact.h"

namespace rippletrace {

struct ReportOptions {
    bool useColor = true;
    int maxImpactRows = 40;
    int maxPathRows = 12;
};

std::string renderImpactText(const CallGraph& g, const ImpactReport& r,
                            const std::vector<std::string>& requestedNames,
                            const ReportOptions& opt = ReportOptions());

std::string renderScanSummary(const CallGraph& g, const ReportOptions& opt = ReportOptions());

std::string renderImpactJson(const CallGraph& g, const ImpactReport& r,
                             const std::vector<std::string>& requestedNames);

// Graphviz digraph of the changed functions and their upstream callers.
std::string renderImpactDot(const CallGraph& g, const ImpactReport& r,
                            int maxNodes = 120);

// Plain list of test function ids to re-run, one per line.
std::string renderTestRunList(const ImpactReport& r);

}  // namespace rippletrace
