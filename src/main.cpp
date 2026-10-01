// ============================================================
//  RippleTrace - main.cpp
//  CLI entry point.
//
//  Commands
//    scan    <repo>                      build the call graph, show summary
//    impact  <repo> --fn <name> [...]    blast radius for named function(s)
//    diff    <repo> --diff <file>        blast radius derived from a patch
//    tests   <repo> --fn <name>          test selection only
//    serve   <root>                      web UI + JSON API
//    prompt  <repo> --fn <name>          print the grounded AI prompt only
// ============================================================
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ai_explain.h"
#include "cli_utf8.h"
#include "diff.h"
#include "graph.h"
#include "impact.h"
#include "json.h"
#include "parser.h"
#include "repo.h"
#include "report.h"
#include "web.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace {

using namespace rippletrace;

// ------------------------------------------------------------
//  UTF-8 command line.
//
//  main(int, char**) receives arguments decoded with the ANSI code page, so a
//  path containing non-ASCII characters arrives mangled:
//      error: no supported sources found under E:\VS??\?????1\...
//  The wide command line is the only lossless source, so on Windows the
//  arguments are re-read from it and converted to UTF-8.
// ------------------------------------------------------------

const char* kVersion = "0.1.0 (concept build)";

void enableAnsi() {
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

// The AI layer answers in Chinese by default, and every path and identifier it
// quotes is UTF-8. A console still in the OEM code page (936 on a Chinese
// Windows) renders those bytes as mojibake, so switch the output code page to
// UTF-8 before anything is printed. Harmless when stdout is a pipe or a file:
// the bytes written are UTF-8 either way, which is what a redirect wants.
void enableUtf8Console() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void printUsage() {
    std::cout <<
        "涟漪 RippleTrace " << kVersion << " - change-impact analyzer for C++ / Java / Python\n"
        "\n"
        "USAGE\n"
        "  RippleTrace scan   <repoDir> [options]\n"
        "  RippleTrace impact <repoDir> --fn <name> [--fn <name>...] [options]\n"
        "  RippleTrace diff   <repoDir> --diff <patchFile> [options]\n"
        "  RippleTrace tests  <repoDir> --fn <name> [options]\n"
        "  RippleTrace prompt <repoDir> --fn <name>       print the grounded AI prompt\n"
        "  RippleTrace serve  <rootDir> [--port <n>]\n"
        "\n"
        "OPTIONS\n"
        "  --fn <name>         function to treat as changed (short or qualified).\n"
        "                      Repeatable. Short names are resolved through the\n"
        "                      scope chain; ambiguous names are reported.\n"
        "  --diff <file>       unified diff (git diff output) whose touched\n"
        "                      functions become the changed set.\n"
        "  --repo <dir>        alternative way to pass the repository root.\n"
        "  --port <n>          port for 'serve' (default 8080).\n"
        "  --max-depth <n>     traversal depth for callers (default 6).\n"
        "  --max-rows <n>      max impacted rows to print (default 40).\n"
        "  --json <file>       machine-readable report, AI section included\n"
        "                      (\"-\" = stdout).\n"
        "  --dot <file>        write Graphviz DOT of the impact subgraph.\n"
        "  --test-list <file>  write the recommended test list, one id per line.\n"
        "\n"
        "  AI LAYER (optional; the impact set stays computed, not guessed)\n"
        "  --ai                enable the explanation layer.\n"
        "  --no-ai             disable it even if a key is configured.\n"
        "  --ai-key <key>      API key (or set RIPPLETRACE_AI_KEY).\n"
        "  --ai-endpoint <url> OpenAI-compatible /chat/completions URL\n"
        "                      (or RIPPLETRACE_AI_ENDPOINT).\n"
        "  --ai-model <name>   model id (or RIPPLETRACE_AI_MODEL).\n"
        "  --ai-lang <code>    language of the model's prose: zh (default),\n"
        "                      en, or auto (or RIPPLETRACE_AI_LANG). Function ids,\n"
        "                      file paths and code identifiers are never\n"
        "                      translated, in any mode.\n"
        "  --print-prompt      print the grounded prompt and exit (no call).\n"
        "\n"
        "  --no-color          disable ANSI colour.\n"
        "  --allow-cross-language\n"
        "                      let a call resolve to a definition in a DIFFERENT\n"
        "                      language. Off by default: measured on a mixed repo,\n"
        "                      this fabricated 26 of 220 edges, all false.\n"
        "  --quiet             suppress the console report.\n"
        "  -h, --help          this text.\n"
        "\n"
        "LANGUAGES\n"
        "  Recognised: C, C++ (.cpp .cc .cxx .hpp .hh .hxx .h .inl .ipp .tpp),\n"
        "              Java (.java), Python (.py .pyw).\n"
        "  Engine per family: brace-scoped (C/C++/Java) and indentation-scoped\n"
        "  (Python). Resolution quality differs per language and is reported.\n"
        "\n"
        "EXAMPLES\n"
        "  RippleTrace scan   samples/tinybank\n"
        "  RippleTrace impact samples/tinybank --fn sha256_hex --fn Money::format\n"
        "  RippleTrace diff   samples/tinybank --diff changes.patch --json report.json\n"
        "  RippleTrace prompt samples/tinybank --fn sha256_hex   (see what the model gets)\n"
        "  set RIPPLETRACE_AI_KEY=sk-... && RippleTrace impact samples/tinybank --fn sha256_hex --ai\n"
        "  RippleTrace serve  samples --port 8080      (then open the URL it prints)\n";
}

struct CliArgs {
    std::string command;
    std::string repo;
    std::vector<std::string> fns;
    std::string diffFile;
    std::string jsonOut;
    std::string dotOut;
    std::string testListOut;
    int maxDepth = 6;
    int maxRows = 40;
    int port = 8080;
    bool noColor = false;
    bool quiet = false;
    bool help = false;
    // ---- AI layer ----
    bool aiFlag = false;            // --ai / --no-ai explicitly requested
    bool aiEnabled = false;         // resolved value
    std::string aiEndpoint;
    std::string aiKey;
    std::string aiModel;
    std::string aiLang;             // --ai-lang
    bool printPromptOnly = false;   // --print-prompt

    bool allowCrossLanguage = false;   // --allow-cross-language


    bool includeNonBuild = false;      // --include-non-build
};

bool parseArgs(const std::vector<std::string>& argv, CliArgs& a) {
    if (argv.size() < 2) { a.help = true; return true; }
    a.command = argv[1];
    if (a.command == "-h" || a.command == "--help" || a.command == "help") {
        a.help = true;
        return true;
    }

    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argv.size()) {
                std::cerr << "error: " << name << " requires a value\n";
                return std::string();
            }
            return std::string(argv[++i]);
        };

        if (arg == "--fn") {
            std::string v = needValue("--fn");
            if (v.empty()) return false;
            a.fns.push_back(v);
        } else if (arg == "--diff") {
            a.diffFile = needValue("--diff");
        } else if (arg == "--repo") {
            a.repo = needValue("--repo");
        } else if (arg == "--max-depth") {
            std::string v = needValue("--max-depth");
            try { a.maxDepth = std::max(1, std::stoi(v)); } catch (...) { return false; }
        } else if (arg == "--max-rows") {
            std::string v = needValue("--max-rows");
            try { a.maxRows = std::max(1, std::stoi(v)); } catch (...) { return false; }
        } else if (arg == "--port") {
            std::string v = needValue("--port");
            try { a.port = std::stoi(v); } catch (...) { return false; }
            if (a.port < 1 || a.port > 65535) {
                std::cerr << "error: --port must be between 1 and 65535\n";
                return false;
            }
        } else if (arg == "--json") {
            a.jsonOut = needValue("--json");
        } else if (arg == "--dot") {
            a.dotOut = needValue("--dot");
        } else if (arg == "--test-list") {
            a.testListOut = needValue("--test-list");
        } else if (arg == "--include-non-build") {

            a.includeNonBuild = true;

        } else if (arg == "--allow-cross-language") {

            a.allowCrossLanguage = true;

        } else if (arg == "--no-color") {
            a.noColor = true;
        } else if (arg == "--ai") {
            a.aiFlag = true;
            a.aiEnabled = true;
        } else if (arg == "--no-ai") {
            a.aiFlag = true;
            a.aiEnabled = false;
        } else if (arg == "--ai-key") {
            a.aiKey = needValue("--ai-key");
        } else if (arg == "--ai-endpoint") {
            a.aiEndpoint = needValue("--ai-endpoint");
        } else if (arg == "--ai-model") {
            a.aiModel = needValue("--ai-model");
        } else if (arg == "--ai-lang") {
            a.aiLang = needValue("--ai-lang");
        } else if (arg == "--print-prompt") {
            a.printPromptOnly = true;
        } else if (arg == "--quiet") {
            a.quiet = true;
        } else if (arg == "-h" || arg == "--help") {
            a.help = true;
            return true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option " << arg << "\n";
            return false;
        } else if (a.repo.empty()) {
            a.repo = arg;
        } else {
            std::cerr << "error: unexpected argument " << arg << "\n";
            return false;
        }
    }
    return true;
}

bool writeFileOrStdout(const std::string& path, const std::string& content) {
    if (path == "-") { std::cout << content; return true; }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write " << path << "\n";
        return false;
    }
    out << content;
    return true;
}

// Build the graph for a repository.
bool buildGraph(const std::string& repo, CallGraph& graph, bool verbose,
                const std::vector<std::string>& onlyLanguages = {},
                bool allowCrossLanguage = false,
                bool includeNonBuildFiles = false) {
    ScanOptions so;
    so.languages = onlyLanguages;
    so.includeNonBuildFiles = includeNonBuildFiles;
    ScanSummary scan;
    std::vector<SourceFile> files = scanRepository(repo, so, scan);
    if (files.empty()) {
        std::cerr << "error: no supported sources found under " << repo
                  << "  (recognised:";
        for (const LanguageProfile* lp : allProfiles()) {
            std::cerr << " " << lp->displayName;
        }
        std::cerr << ")\n";
        return false;
    }

    // Say plainly when a build file decided the file set, and how much was left
    // out. Silently analysing a different set of files than the build uses is
    // how a report ends up pointing at dead code.
    if (scan.vsProjectFound && verbose) {
        std::cerr << "[RippleTrace] Visual Studio project: "
                  << scan.vsProjectPath << "\n";
        std::cerr << "[RippleTrace]   " << scan.fromBuild << " file(s) are in the build";
        if (scan.excludedNotInBuild > 0 && !includeNonBuildFiles) {
            std::cerr << ", " << scan.excludedNotInBuild
                      << " source file(s) on disk are NOT compiled and were skipped\n";
        } else if (scan.excludedNotInBuild > 0) {
            std::cerr << ", " << scan.excludedNotInBuild
                      << " file(s) on disk are NOT compiled but were INCLUDED on request\n";
        } else {
            std::cerr << "\n";
        }
    }

    // One parser per language, created on demand and cached: the engine is
    // chosen by the profile, so adding a language is a data change.
    std::map<std::string, std::unique_ptr<IRepoParser>> parsers;
    std::map<std::string, size_t> perLanguage;
    size_t parsed = 0, failed = 0;

    for (const auto& sf : files) {
        if (sf.language == nullptr) continue;
        std::string text;
        if (!readFileText(sf.absolutePath, text)) { ++failed; continue; }

        auto it = parsers.find(sf.language->id);
        if (it == parsers.end()) {
            it = parsers.emplace(sf.language->id, makeParser(*sf.language)).first;
        }
        if (!it->second) { ++failed; continue; }

        ParseResult pr;
        it->second->parseUnit(sf.repoRelativePath, text, sf.isHeader, pr);
        graph.addParse(pr);
        graph.noteLanguageFile(sf.language->displayName);
        ++parsed;
        ++perLanguage[sf.language->displayName];
    }
    graph.resolve(allowCrossLanguage);

    if (verbose) {
        std::cerr << "[RippleTrace] scanned " << parsed << " file(s)"
                  << (failed ? (" (" + std::to_string(failed) + " unreadable)") : "");
        if (perLanguage.size() > 1) {
            std::cerr << "  [";
            bool first = true;
            for (const auto& kv : perLanguage) {
                if (!first) std::cerr << ", ";
                std::cerr << kv.first << " " << kv.second;
                first = false;
            }
            std::cerr << "]";
        }
        std::cerr << ", " << graph.functionCount() << " functions, "
                  << graph.edges().size() << " edges\n";
    }
    return true;
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
    // Re-read the command line as UTF-8; the CRT-decoded argv mangles
    // non-ASCII paths on Windows.
    std::vector<std::string> args = utf8Args();
    if (args.empty()) return 2;
    enableAnsi();
    enableUtf8Console();

    CliArgs a;
    if (!parseArgs(args, a)) {
        printUsage();
        return 2;
    }
    if (a.help) { printUsage(); return 0; }

    if (a.command != "scan" && a.command != "impact" &&
        a.command != "diff" && a.command != "tests" && a.command != "serve" &&
        a.command != "prompt") {
        std::cerr << "error: unknown command '" << a.command << "'\n\n";
        printUsage();
        return 2;
    }
    if (a.repo.empty()) {
        std::cerr << "error: repository directory is required\n\n";
        printUsage();
        return 2;
    }

    // ---------------- serve (web UI) ----------------
    // Handled before any graph work: the server builds graphs per request.
    if (a.command == "serve") {
        std::error_code ec;
        // a.repo is UTF-8 (see utf8Args); building a filesystem::path from it
        // directly would decode with the ANSI code page and fail for a path
        // containing non-ASCII characters.
        if (!std::filesystem::exists(utf8ToPath(a.repo), ec)) {
            std::cerr << "error: directory not found: " << a.repo << "\n";
            return 2;
        }
        ServeOptions options;
        options.port = a.port;
        options.allowedRoot = a.repo;
        options.verbose = !a.quiet;
        return runWebServer(options);
    }

    ReportOptions ropt;
    ropt.useColor = !a.noColor;
    ropt.maxImpactRows = a.maxRows;

    CallGraph graph;
    if (!buildGraph(a.repo, graph, !a.quiet, {}, a.allowCrossLanguage, a.includeNonBuild)) return 1;

    // ---------------- scan ----------------
    if (a.command == "scan") {
        // 'scan' reports graph-level statistics; there is no change set yet.
        // Flags that only mean something once a change set exists are rejected
        // rather than silently ignored: a flag that quietly writes no file is
        // worse than one that refuses, because the user cannot tell.
        if (!a.dotOut.empty()) {
            std::cerr << "error: --dot describes an impact subgraph, so it needs a change set.\n"
                         "       use 'impact' or 'diff' instead of 'scan'.\n";
            return 2;
        }
        if (!a.testListOut.empty()) {
            std::cerr << "error: --test-list needs a change set.\n"
                         "       use 'impact', 'diff' or 'tests' instead of 'scan'.\n";
            return 2;
        }
        if (!a.fns.empty()) {
            std::cerr << "error: 'scan' does not accept --fn.\n"
                         "       use 'impact' or 'tests' to analyse a named function.\n";
            return 2;
        }
        if (a.aiFlag || a.aiEnabled) {
            std::cerr << "error: the AI layer explains an impact set, so it needs a change set.\n"
                         "       use 'impact' or 'diff' instead of 'scan'.\n";
            return 2;
        }
        if (!a.quiet) std::cout << renderScanSummary(graph, ropt);
        if (!a.jsonOut.empty()) {
            // scan JSON is a thin projection: function list with fan-in
            std::ostringstream o;
            o << "{\n  \"tool\": \"rippletrace\",\n  \"schemaVersion\": 1,\n";
            GraphStats st = graph.stats();
            o << "  \"summary\": {\"functions\": " << st.functions
              << ", \"edges\": " << st.edges
              << ", \"unresolvedCallees\": " << st.unresolvedEdges
              << ", \"tests\": " << st.tests
              << ", \"entryPoints\": " << st.entryPoints
              << ", \"publicApi\": " << st.publicApi << "},\n";
            o << "  \"functions\": [\n";
            auto fns = graph.allFunctions();
            for (size_t i = 0; i < fns.size(); ++i) {
                const RepoFunction* f = fns[i];
                o << "    {\"id\": \"" << f->qualified << "\", \"file\": \"" << f->file
                  << "\", \"line\": " << f->declLine
                  << ", \"fanIn\": " << graph.callersOf(f->qualified).size()
                  << ", \"fanOut\": " << f->directCallCount
                  << ", \"lines\": " << f->lineCount
                  << ", \"cyclomatic\": " << f->cyclomatic
                  << ", \"isTest\": " << (f->isTest ? "true" : "false")
                  << ", \"isPublicApi\": " << (f->isPublicApi ? "true" : "false")
                  << "}" << (i + 1 < fns.size() ? "," : "") << "\n";
            }
            o << "  ]\n}\n";
            if (!writeFileOrStdout(a.jsonOut, o.str())) return 1;
        }
        return 0;
    }

    // ---------------- determine changed set ----------------
    std::vector<std::string> requested = a.fns;
    bool derivedFromDiff = false;

    if (a.command == "diff" || !a.diffFile.empty()) {
        if (a.diffFile.empty()) {
            std::cerr << "error: --diff <patchFile> is required for 'diff'\n";
            return 2;
        }
        std::string patch;
        if (!readFileText(a.diffFile, patch)) {
            std::cerr << "error: cannot read patch " << a.diffFile << "\n";
            return 1;
        }
        DiffParseResult d = parseUnifiedDiff(patch);
        if (!d.warnings.empty() && !a.quiet) {
            for (const auto& w : d.warnings) std::cerr << "[RippleTrace] diff: " << w << "\n";
        }
        auto touched = functionsTouchedByDiff(graph, d);
        if (!a.quiet) {
            std::cout << "Patch touches " << d.files.size() << " file(s), +"
                      << d.totalAdded << " -" << d.totalRemoved << " line(s); "
                      << touched.size() << " function body(ies) modified.\n";
        }
        requested.clear();
        for (const auto& t : touched) {
            requested.push_back(t.qualified);
            if (!a.quiet) {
                std::cout << "  * " << t.qualified << "  " << t.file << ":" << t.bodyStart
                          << "-" << t.bodyEnd << "  (" << t.addedLinesInBody
                          << " added line" << (t.addedLinesInBody == 1 ? "" : "s") << ")\n";
            }
        }
        if (!a.quiet) {
            // Show the added ranges in OLD-file coordinates: those are the line
            // numbers the graph was built against, so they are what the
            // attribution below used.
            std::cout << "  [attributed added ranges, current-tree line numbers: ";
            bool first = true;
            for (const auto& f : d.files) {
                for (const auto& r : f.addedLineRangesOldSide) {
                    if (!first) std::cout << ", ";
                    std::cout << r.first << "-" << r.second;
                    first = false;
                }
            }
            std::cout << "]\n";
        }
        if (requested.empty()) {
            std::cerr << "error: no function body overlaps the patch hunks; "
                         "nothing to analyse. Check that the patch is a unified diff "
                         "for this repository.\n";
            return 1;
        }
        derivedFromDiff = true;
        if (!a.quiet) std::cout << "\n";
    }

    if (requested.empty()) {
        std::cerr << "error: provide --fn <name> or --diff <patchFile>\n";
        return 2;
    }

    // ---------------- analyse ----------------
    ImpactOptions iopt;
    iopt.maxDepth = a.maxDepth;
    ImpactAnalyzer analyzer(graph);
    ImpactReport rep = analyzer.analyze(requested, iopt);

    // Warn when a "changed" name was not found: impact would silently be empty.
    if (!rep.unknownChanged.empty() && !a.quiet) {
        std::cerr << "[RippleTrace] warning: " << rep.unknownChanged.size()
                  << " name(s) not found in the graph:";
        for (const auto& u : rep.unknownChanged) std::cerr << " " << u;
        std::cerr << "\n";
    }

    if (!a.quiet) {
        if (a.command == "tests") {
            std::cout << "Recommended tests (" << rep.tests.size() << "):\n";
            for (const auto& t : rep.tests) {
                std::cout << "  " << t.qualified << "   " << t.file << "  "
                          << t.distance << " hop" << (t.distance == 1 ? "" : "s") << "\n";
            }
            if (rep.tests.empty()) {
                std::cout << "  (none found: no test function reaches the changed code)\n";
            }
        } else if (a.command == "prompt") {
            // The prompt is the whole output of this command; skip the report
            // so the prompt can be piped or copied without noise around it.
        } else {
            std::cout << renderImpactText(graph, rep, requested, ropt);
        }
    }

    // ---------------- AI layer ----------------
    // The impact set above is computed and final. The model is only allowed to
    // annotate it; anything it invents is rejected in enforceGrounding().
    AiOptions aiOpt = loadAiOptionsFromEnv();
    if (!a.aiEndpoint.empty()) aiOpt.endpoint = a.aiEndpoint;
    if (!a.aiKey.empty()) { aiOpt.apiKey = a.aiKey; aiOpt.enabled = true; }
    if (!a.aiModel.empty()) aiOpt.model = a.aiModel;
    if (!a.aiLang.empty()) aiOpt.language = a.aiLang;
    if (a.aiFlag) aiOpt.enabled = a.aiEnabled;
    aiOpt.verbose = !a.quiet;

    // 'prompt' command and --print-prompt: show the grounded prompt and stop.
    if (a.command == "prompt" || a.printPromptOnly) {
        std::cout << "=== SYSTEM ===\n" << buildSystemPrompt(aiOpt.language) << "\n";
        std::cout << "=== USER (grounded context) ===\n"
                  << buildExplanationPrompt(graph, rep, aiOpt);
        std::cout << "\n=== NOTE ===\n"
                  << "This prompt contains only facts produced by the static analysis.\n"
                  << "The model's reply is filtered against that set in "
                     "enforceGrounding().\n";
        return 0;
    }

    AiExplanation aiEx;
    if (aiOpt.enabled) {
        auto client = makeChatClient(aiOpt);
        aiEx = explainImpact(graph, rep, aiOpt, client.get());
    } else {
        // Still build the prompt so its size is reported, but never fabricate.
        aiEx = explainImpact(graph, rep, aiOpt, nullptr);
    }

    if (!a.quiet && a.command != "prompt" && a.command != "tests") {
        if (aiOpt.enabled) {
            std::cout << renderAiExplanationText(aiEx, ropt.useColor);
        } else {
            // Say why the AI section is missing instead of silently omitting it:
            // silence is indistinguishable from "the model found nothing".
            std::cout << "\n\x1b[2m"
                      << "AI review guidance: not requested. This run was fully "
                         "offline and deterministic.\n"
                      << "  enable with:  --ai --ai-key <key>   "
                         "(or set RIPPLETRACE_AI_KEY)\n"
                      << "  inspect the prompt first:  RippleTrace prompt <repo> --fn <name>"
                      << "\x1b[0m\n";
        }
    }

    if (!a.jsonOut.empty()) {
        // Compose the impact report and attach the AI section, so machine
        // consumers get one document rather than two.
        json::Value root = json::parseOrNull(renderImpactJson(graph, rep, requested));
        if (!root.isObject()) root = json::Value::makeObject();
        appendAiExplanationJson(root, aiEx);
        if (!writeFileOrStdout(a.jsonOut, json::serialize(root, true))) return 1;
        if (a.jsonOut != "-" && !a.quiet) {
            std::cout << "[RippleTrace] wrote " << a.jsonOut << "\n";
        }
    }
    if (!a.dotOut.empty()) {
        if (!writeFileOrStdout(a.dotOut, renderImpactDot(graph, rep))) return 1;
        if (a.dotOut != "-" && !a.quiet) {
            std::cout << "[RippleTrace] wrote " << a.dotOut
                      << "  (render: dot -Tsvg " << a.dotOut << " -o impact.svg)\n";
        }
    }
    if (!a.testListOut.empty()) {
        if (!writeFileOrStdout(a.testListOut, renderTestRunList(rep))) return 1;
        if (a.testListOut != "-" && !a.quiet) {
            std::cout << "[RippleTrace] wrote " << a.testListOut << "\n";
        }
    }

    (void)derivedFromDiff;
    return 0;
}
