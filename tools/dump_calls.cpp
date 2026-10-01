// Diagnostic: print every resolved edge grouped by caller, to answer questions
// like "does main@python actually reach build_default_services?".
//
// Usage: dump_calls.exe <repoDir> [callerSubstring]
#include <algorithm>
#include <cstdio>
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
        std::fprintf(stderr, "usage: dump_calls <repoDir> [callerSubstring]\n");
        return 2;
    }
    const std::string root = args[1];
    const std::string filter = (args.size() > 2) ? args[2] : std::string();

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

    std::map<std::string, std::vector<std::string>> byCaller;
    for (const auto& e : graph.edges()) {
        byCaller[e.caller].push_back(e.calleeQualified);
    }

    for (const auto& kv : byCaller) {
        if (!filter.empty() && kv.first.find(filter) == std::string::npos) continue;
        std::printf("%s\n", kv.first.c_str());
        std::vector<std::string> sorted = kv.second;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (const auto& t : sorted) {
            std::printf("    -> %s\n", t.c_str());
        }
    }

    // Also report who calls a given name, if provided.
    if (!filter.empty()) {
        std::printf("\ncallers of anything containing '%s':\n", filter.c_str());
        for (const auto& e : graph.edges()) {
            if (e.calleeQualified.find(filter) != std::string::npos) {
                std::printf("    %s -> %s\n", e.caller.c_str(), e.calleeQualified.c_str());
            }
        }
    }
    return 0;
}
