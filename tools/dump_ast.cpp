// ============================================================
//  RippleTrace - dump_ast.cpp
//  Per-file extraction report, for measuring the extractors against an
//  independent parser (tree-sitter). Prints counts on lines beginning with
//  DEFS / CALLS so a script can read them, then lists what was found.
//
//  Usage: dump_ast.exe <file> [--verbose]
// ============================================================
#include <cstdio>
#include <string>

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
        std::fprintf(stderr, "usage: dump_ast <file> [--verbose]\n");
        return 2;
    }
    const std::string path = args[1];
    const bool verbose = (args.size() > 2 && std::string(args[2]) == "--verbose");

    const LanguageProfile* lang = profileForPath(path);
    if (!lang) {
        std::fprintf(stderr, "unsupported file type: %s\n", path.c_str());
        return 3;
    }

    std::string text;
    if (!readFileText(path, text)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 4;
    }

    auto parser = makeParser(*lang);
    if (!parser) {
        std::fprintf(stderr, "no parser for language %s\n", lang->id.c_str());
        return 5;
    }

    ParseResult pr;
    parser->parseUnit(path, text, isHeaderPathFor(*lang, path), pr);

    if (verbose && std::getenv("RIPPLETRACE_SHOW_TEXT")) {
        // Show the decoded text so encoding handling can be eyeballed.
        std::printf("--- decoded first 12 lines ---\n");
        int lineNo = 1;
        size_t start = 0;
        while (start <= text.size() && lineNo <= 12) {
            size_t nl = text.find('\n', start);
            std::string ln = text.substr(start, nl == std::string::npos ? std::string::npos
                                                                       : nl - start);
            std::printf("%3d| %s\n", lineNo, ln.c_str());
            if (nl == std::string::npos) break;
            start = nl + 1;
            ++lineNo;
        }
        std::printf("--- end ---\n");
    }
    std::printf("LANG %s\n", lang->id.c_str());
    std::printf("DEFS %zu\n", pr.functions.size());
    std::printf("CALLS %zu\n", pr.edges.size());

    if (verbose) {
        for (const auto& f : pr.functions) {
            std::printf("  def  %-44s  %s:%d  (%d lines)\n",
                        f.qualified.c_str(), f.file.c_str(), f.declLine, f.lineCount);
        }
        for (const auto& e : pr.edges) {
            std::printf("  call %-30s -> %-30s  %s:%d\n",
                        e.caller.c_str(), e.calleeQualified.c_str(),
                        e.evidence.file.c_str(), e.evidence.line);
        }
        for (const auto& w : pr.warnings) {
            std::printf("  warn %s\n", w.c_str());
        }
    }
    return 0;
}
