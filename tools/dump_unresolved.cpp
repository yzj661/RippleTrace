// Diagnostic: dump unresolved call sites so the "N unresolved" number can be
// explained rather than hand-waved. A claim like "most of them are std::" has
// to be measured, not asserted.
//
// Usage: dump_unresolved.exe <repoDir> [topN]
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "graph.h"
#include "cli_utf8.h"
#include "parser.h"
#include "repo.h"

using namespace rippletrace;

int main(int argc, char** argv) {
    setFallbackArgs(argc, argv);
    const std::vector<std::string> args = utf8Args();
    const size_t argc2 = args.size();
    (void)argc2;
    if (args.size() < 2) {
        std::printf("usage: dump_unresolved <repoDir> [topN]\n");
        return 2;
    }
    const std::string root = args[1];
    const int topN = (args.size() > 2) ? std::atoi(args[2].c_str()) : 40;

    ScanOptions so;
    auto files = scanRepository(root, so);

    HeuristicParser parser;   // C++ only diagnostic
    CallGraph graph;
    for (const auto& f : files) {
        std::string text;
        if (!readFileText(f.absolutePath, text)) continue;
        ParseResult pr;
        parser.parseUnit(f.repoRelativePath, text, f.isHeader, pr);
        graph.addParse(pr);
    }
    graph.resolve();

    // Re-run resolution to classify every call site.
    std::map<std::string, int> unresolvedByName;
    int total = 0, resolved = 0;

    // The graph keeps only resolved edges, so re-parse to see all call sites.
    for (const auto& f : files) {
        std::string text;
        if (!readFileText(f.absolutePath, text)) continue;
        ParseResult pr;
        parser.parseUnit(f.repoRelativePath, text, f.isHeader, pr);
        for (const auto& e : pr.edges) {
            ++total;
            // A call is resolved when the graph has an edge with this caller and
            // this callee short name.
            bool found = false;
            for (const auto& ge : graph.edges()) {
                if (ge.caller == e.caller && ge.callee == e.callee) { found = true; break; }
            }
            if (found) { ++resolved; continue; }
            unresolvedByName[e.calleeQualified]++;
        }
    }

    std::printf("call sites parsed   : %d\n", total);
    std::printf("resolved to a def   : %d\n", resolved);
    std::printf("unresolved          : %d\n\n", total - resolved);

    std::vector<std::pair<std::string, int>> rows(unresolvedByName.begin(),
                                                  unresolvedByName.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    // Classify by shape so the summary is meaningful.
    int stdCalls = 0, memberCalls = 0, plainCalls = 0, otherCalls = 0;
    for (const auto& r : rows) {
        const std::string& n = r.first;
        if (n.rfind("std::", 0) == 0 || n.find("::") != std::string::npos) {
            if (n.rfind("std::", 0) == 0) stdCalls += r.second;
            else ++otherCalls;
        } else if (n.find('.') != std::string::npos) {
            memberCalls += r.second;
        } else {
            plainCalls += r.second;
        }
    }
    std::printf("classification of unresolved call SITES:\n");
    std::printf("  std::* qualified   : %d\n", stdCalls);
    std::printf("  other qualified    : %d (distinct %d)\n", otherCalls, otherCalls);
    std::printf("  member-ish (a.b)   : %d\n", memberCalls);
    std::printf("  plain identifiers  : %d\n", plainCalls);
    std::printf("\ntop %d unresolved callee names (by occurrence):\n", topN);
    for (int i = 0; i < topN && i < static_cast<int>(rows.size()); ++i) {
        std::printf("  %5d  %s\n", rows[i].second, rows[i].first.c_str());
    }
    return 0;
}
