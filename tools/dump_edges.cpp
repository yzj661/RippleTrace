// ============================================================
//  RippleTrace - dump_edges.cpp
//  Dump every resolved call edge with the language of both endpoints, so the
//  cross-language contamination can be measured instead of guessed at.
//
//  Usage: dump_edges.exe <repoDir> [crosslangOnly]
// ============================================================
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "graph.h"
#include "cli_utf8.h"
#include "lang.h"
#include "parser.h"
#include "repo.h"

using namespace rippletrace;

int main(int argc, char** argv) {
    setFallbackArgs(argc, argv);
    const std::vector<std::string> args = utf8Args();
    const size_t argc2 = args.size();
    (void)argc2;
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: dump_edges <repoDir> [--cross]\n");
        return 2;
    }
    const std::string root = args[1];
    const bool crossOnly = (args.size() > 2 && std::string(args[2]) == "--cross");

    ScanOptions so;
    auto files = scanRepository(root, so);

    std::map<std::string, std::unique_ptr<IRepoParser>> parsers;
    CallGraph graph;
    for (const auto& f : files) {
        if (!f.language) continue;
        std::string text;
        if (!readFileText(f.absolutePath, text)) continue;
        auto it = parsers.find(f.language->id);
        if (it == parsers.end()) {
            it = parsers.emplace(f.language->id, makeParser(*f.language)).first;
        }
        if (!it->second) continue;
        ParseResult pr;
        it->second->parseUnit(f.repoRelativePath, text, f.isHeader, pr);
        graph.addParse(pr);
        graph.noteLanguageFile(f.language->displayName);
    }
    graph.resolve();

    auto langOf = [&](const std::string& id) -> std::string {
        const RepoFunction* fn = graph.find(id);
        if (!fn) return "?";
        const LanguageProfile* lp = profileForPath(fn->file);
        return lp ? lp->displayName : "?";
    };

    int total = 0, cross = 0, same = 0;
    std::map<std::string, int> pairs;
    std::vector<std::string> crossEdges;

    for (const auto& e : graph.edges()) {
        ++total;
        const std::string a = langOf(e.caller);
        const std::string b = langOf(e.calleeQualified);
        if (a == "?" || b == "?") continue;
        if (a == b) {
            ++same;
        } else {
            ++cross;
            pairs[a + " -> " + b]++;
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%-34s [%s]  ->  %-34s [%s]",
                          e.caller.c_str(), a.c_str(),
                          e.calleeQualified.c_str(), b.c_str());
            crossEdges.push_back(buf);
        }
    }

    std::printf("resolved edges        : %d\n", total);
    std::printf("  same-language       : %d\n", same);
    std::printf("  CROSS-language      : %d\n", cross);
    if (!pairs.empty()) {
        std::printf("\ncross-language pairs:\n");
        for (const auto& kv : pairs) {
            std::printf("  %-20s %d\n", kv.first.c_str(), kv.second);
        }
        std::printf("\ncross-language edges:\n");
        for (size_t i = 0; i < crossEdges.size() && i < 40; ++i) {
            std::printf("  %s\n", crossEdges[i].c_str());
        }
    }

    if (crossOnly) return cross > 0 ? 1 : 0;
    return 0;
}
