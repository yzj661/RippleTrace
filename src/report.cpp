// ============================================================
//  RippleTrace - report.cpp
// ============================================================
#include "report.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace rippletrace {

namespace {

const char* RESET = "\x1b[0m";
const char* BOLD = "\x1b[1m";
const char* DIM = "\x1b[2m";
const char* RED = "\x1b[31m";
const char* YELLOW = "\x1b[33m";
const char* GREEN = "\x1b[32m";
const char* CYAN = "\x1b[36m";

struct Palette {
    bool on;
    const char* bold() const { return on ? BOLD : ""; }
    const char* dim() const { return on ? DIM : ""; }
    const char* reset() const { return on ? RESET : ""; }
    const char* critical() const { return on ? RED : ""; }
    const char* high() const { return on ? YELLOW : ""; }
    const char* medium() const { return on ? CYAN : ""; }
    const char* low() const { return on ? GREEN : ""; }
    const char* risk(Risk r) const {
        switch (r) {
            case Risk::Critical: return critical();
            case Risk::High: return high();
            case Risk::Medium: return medium();
            default: return low();
        }
    }
};

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string shortId(const std::string& qualified, size_t maxLen = 46) {
    if (qualified.size() <= maxLen) return qualified;
    return qualified.substr(0, maxLen - 3) + "...";
}

std::string pathString(const std::vector<std::string>& path) {
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) out += " -> ";
        out += path[i];
    }
    return out;
}

std::string bar(int score) {
    int n = std::max(0, std::min(20, score / 5));
    std::string s = "[";
    for (int i = 0; i < 20; ++i) s += (i < n ? '#' : '.');
    s += "]";
    return s;
}

std::string lineOf(size_t n, char c) { return std::string(n, c); }

}  // namespace

std::string renderScanSummary(const CallGraph& g, const ReportOptions& opt) {
    Palette p{opt.useColor};
    GraphStats st = g.stats();
    std::ostringstream o;

    o << p.bold() << "涟漪 RippleTrace scan summary" << p.reset() << "\n";
    o << lineOf(64, '-') << "\n";
    o << "  functions defined     : " << st.functions << "\n";
    o << "  resolved call edges   : " << st.edges << "\n";
    o << "  unresolved call sites : " << st.unresolvedEdges
      << "   (external / template / ambiguous)\n";
    o << "  test functions        : " << st.tests << "\n";
    o << "  entry points          : " << st.entryPoints << "\n";
    o << "  defs in public headers: " << st.publicApi
      << "   (bodies in headers only; declaration-only headers count 0)\n";
    o << "  ambiguous short names : " << st.ambiguousNames << "\n";

    // Per-language breakdown. With more than one language in play a single
    // total hides where the numbers came from, and resolution quality differs
    // a lot between languages - so say so rather than blending them.
    if (st.perLanguage.size() > 1) {
        o << "\n" << p.bold() << "By language" << p.reset() << "\n";
        o << "  " << std::left << std::setw(12) << "language"
          << std::right << std::setw(8) << "files"
          << std::setw(12) << "functions" << "\n";
        for (const auto& kv : st.perLanguage) {
            o << "  " << std::left << std::setw(12) << kv.first
              << std::right << std::setw(8) << kv.second.files
              << std::setw(12) << kv.second.functions << "\n";
        }
        o << "\n" << p.dim() << "  how well calls resolve, per language:"
          << p.reset() << "\n";
        for (const auto& kv : st.perLanguage) {
            const LanguageProfile* lp = nullptr;
            for (const LanguageProfile* cand : allProfiles()) {
                if (cand->displayName == kv.first) { lp = cand; break; }
            }
            if (lp == nullptr) continue;
            o << p.dim() << "    " << lp->displayName << ": " << lp->resolutionNote
              << p.reset() << "\n";
        }
    }

    // top fan-in functions: the highest-blast-radius nodes in the repo
    struct Row { std::string id; int fanIn; int size; };
    std::vector<Row> rows;
    for (const RepoFunction* f : g.allFunctions()) {
        int fi = static_cast<int>(g.callersOf(f->qualified).size());
        rows.push_back({f->qualified, fi, f->lineCount});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.fanIn != b.fanIn) return a.fanIn > b.fanIn;
        return a.id < b.id;
    });

    o << "\n" << p.bold() << "Top blast-radius functions (by direct fan-in)"
      << p.reset() << "\n";
    o << "  " << std::left << std::setw(48) << "function" << std::right
      << std::setw(7) << "fan-in" << std::setw(8) << "lines" << "\n";
    for (size_t i = 0; i < rows.size() && i < 12; ++i) {
        o << "  " << std::left << std::setw(48) << shortId(rows[i].id) << std::right
          << std::setw(7) << rows[i].fanIn << std::setw(8) << rows[i].size << "\n";
    }

    if (!g.warnings().empty()) {
        o << "\n" << p.bold() << "Parser warnings (" << g.warnings().size() << ")"
          << p.reset() << "\n";
        for (size_t i = 0; i < g.warnings().size() && i < 10; ++i) {
            o << "  - " << g.warnings()[i] << "\n";
        }
    }
    return o.str();
}

std::string renderImpactText(const CallGraph& g, const ImpactReport& r,
                             const std::vector<std::string>& requestedNames,
                             const ReportOptions& opt) {
    Palette p{opt.useColor};
    std::ostringstream o;

    o << p.bold() << "涟漪 RippleTrace change-impact report" << p.reset() << "\n";
    o << lineOf(72, '=') << "\n";

    // ---- head line: what changed ----
    o << p.bold() << "Changed " << p.reset();
    if (!requestedNames.empty()) {
        o << "(" << requestedNames.size() << "): ";
        for (size_t i = 0; i < requestedNames.size(); ++i) {
            if (i) o << ", ";
            o << p.bold() << requestedNames[i] << p.reset();
        }
    }
    o << "\n";
    if (!r.unknownChanged.empty()) {
        o << p.high() << "  ! unresolved: ";
        for (size_t i = 0; i < r.unknownChanged.size(); ++i) {
            if (i) o << ", ";
            o << r.unknownChanged[i];
        }
        o << p.reset() << "  (not defined in this repo)\n";
    }

    // ---- headline metrics ----
    int critical = 0, high = 0, medium = 0, low = 0;
    for (const auto& im : r.impacted) {
        switch (im.risk) {
            case Risk::Critical: ++critical; break;
            case Risk::High: ++high; break;
            case Risk::Medium: ++medium; break;
            default: ++low; break;
        }
    }
    o << "\n" << p.bold() << "Blast radius" << p.reset() << "\n";
    o << "  impacted functions : " << r.impacted.size()
      << "   (max depth reached " << r.maxDepthReached << ")\n";
    o << "  by risk            : "
      << p.critical() << critical << " critical" << p.reset() << ", "
      << p.high() << high << " high" << p.reset() << ", "
      << p.medium() << medium << " medium" << p.reset() << ", "
      << p.low() << low << " low" << p.reset() << "\n";
    o << "  tests to re-run    : " << r.tests.size() << "\n";
    o << "  change interactions: " << r.conflicts.size() << "\n";

    // ---- ranked impacted list ----
    o << "\n" << p.bold() << "Ranked impact (risk = fan-in x distance x exposure)"
      << p.reset() << "\n";
    o << "  " << std::left << std::setw(9) << "risk" << std::setw(6) << "score"
      << std::setw(6) << "depth" << std::setw(7) << "fan-in" << "function"
      << p.reset() << "\n";
    o << lineOf(72, '-') << "\n";

    size_t shown = 0;
    for (const auto& im : r.impacted) {
        if (shown >= static_cast<size_t>(opt.maxImpactRows)) break;
        ++shown;
        std::ostringstream scoreStr;
        scoreStr << im.riskScore;
        o << "  " << p.risk(im.risk) << std::left << std::setw(9) << riskName(im.risk)
          << p.reset() << std::left << std::setw(6) << scoreStr.str()
          << std::setw(6) << im.depth << std::setw(7) << im.fanIn
          << shortId(im.qualified) << (im.isTest ? "  [test]" : "") << "\n";
        o << "  " << p.dim() << "        " << bar(im.riskScore) << "  " << im.file
          << ":" << im.line << p.reset() << "\n";
        for (const auto& reason : im.reasons) {
            o << "  " << p.dim() << "        - " << reason << p.reset() << "\n";
        }
    }
    if (r.impacted.size() > shown) {
        o << "  " << p.dim() << "  ... " << (r.impacted.size() - shown)
          << " more (raise --max-rows to see)" << p.reset() << "\n";
    }

    // ---- why: concrete call chains ----
    if (!r.impacted.empty()) {
        o << "\n" << p.bold() << "Why (shortest call path change -> impact)"
          << p.reset() << "\n";
        int n = 0;
        for (const auto& im : r.impacted) {
            if (im.path.size() < 2) continue;
            if (n++ >= opt.maxPathRows) break;
            o << "  " << p.dim() << "[" << riskName(im.risk) << "] " << p.reset()
              << pathString(im.path) << "\n";
        }
    }

    // ---- change interactions ----
    if (!r.conflicts.empty()) {
        o << "\n" << p.bold() << "Change interactions (cannot be validated separately)"
          << p.reset() << "\n";
        for (const auto& cp : r.conflicts) {
            o << "  " << p.high() << "!" << p.reset() << " " << cp.a << "  <->  " << cp.b
              << "\n";
            o << "      " << cp.note;
            if (!cp.examples.empty()) {
                o << "; e.g. " << cp.examples.front();
            }
            o << "\n";
        }
    }

    // ---- test run list ----
    if (!r.tests.empty()) {
        o << "\n" << p.bold() << "Recommended test selection" << p.reset() << "\n";
        for (const auto& t : r.tests) {
            o << "  " << (t.distance == 0 ? p.critical() : p.medium()) << "T"
              << p.reset() << " " << t.qualified << "  " << p.dim() << "(" << t.file
              << ", " << t.distance << " hop" << (t.distance == 1 ? "" : "s") << ")"
              << p.reset() << "\n";
        }
    }

    // ---- honest caveats ----
    if (!r.ambiguityNotes.empty()) {
        o << "\n" << p.bold() << "Limitations" << p.reset() << "\n";
        for (const auto& note : r.ambiguityNotes) {
            o << "  * " << note << "\n";
        }
    }

    o << "\n" << lineOf(72, '=') << "\n";
    return o.str();
}

std::string renderImpactJson(const CallGraph& g, const ImpactReport& r,
                             const std::vector<std::string>& requestedNames) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"tool\": \"rippletrace\",\n";
    o << "  \"schemaVersion\": 1,\n";

    o << "  \"requested\": [";
    for (size_t i = 0; i < requestedNames.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << jsonEscape(requestedNames[i]) << "\"";
    }
    o << "],\n";

    o << "  \"changed\": [";
    for (size_t i = 0; i < r.changed.size(); ++i) {
        if (i) o << ", ";
        const RepoFunction* f = g.find(r.changed[i]);
        o << "{\"id\": \"" << jsonEscape(r.changed[i]) << "\"";
        if (f) o << ", \"file\": \"" << jsonEscape(f->file) << "\", \"line\": " << f->declLine;
        o << "}";
    }
    o << "],\n";

    o << "  \"unknown\": [";
    for (size_t i = 0; i < r.unknownChanged.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << jsonEscape(r.unknownChanged[i]) << "\"";
    }
    o << "],\n";

    o << "  \"summary\": {\n";
    o << "    \"impactedCount\": " << r.impacted.size() << ",\n";
    o << "    \"maxDepth\": " << r.maxDepthReached << ",\n";
    o << "    \"testCount\": " << r.tests.size() << ",\n";
    o << "    \"conflictCount\": " << r.conflicts.size() << ",\n";
    o << "    \"resolvedCallees\": " << r.resolvedCalleeCount << ",\n";
    o << "    \"unresolvedCallees\": " << r.unresolvedCalleeCount << "\n";
    o << "  },\n";

    o << "  \"impacted\": [\n";
    for (size_t i = 0; i < r.impacted.size(); ++i) {
        const auto& im = r.impacted[i];
        o << "    {\n";
        o << "      \"id\": \"" << jsonEscape(im.qualified) << "\",\n";
        o << "      \"file\": \"" << jsonEscape(im.file) << "\",\n";
        o << "      \"line\": " << im.line << ",\n";
        o << "      \"depth\": " << im.depth << ",\n";
        o << "      \"fanIn\": " << im.fanIn << ",\n";
        o << "      \"risk\": \"" << riskName(im.risk) << "\",\n";
        o << "      \"riskScore\": " << im.riskScore << ",\n";
        o << "      \"isTest\": " << (im.isTest ? "true" : "false") << ",\n";
        o << "      \"isPublicApi\": " << (im.isPublicApi ? "true" : "false") << ",\n";
        o << "      \"reasons\": [";
        for (size_t k = 0; k < im.reasons.size(); ++k) {
            if (k) o << ", ";
            o << "\"" << jsonEscape(im.reasons[k]) << "\"";
        }
        o << "],\n";
        o << "      \"path\": [";
        for (size_t k = 0; k < im.path.size(); ++k) {
            if (k) o << ", ";
            o << "\"" << jsonEscape(im.path[k]) << "\"";
        }
        o << "]\n";
        o << "    }" << (i + 1 < r.impacted.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"tests\": [\n";
    for (size_t i = 0; i < r.tests.size(); ++i) {
        o << "    {\"id\": \"" << jsonEscape(r.tests[i].qualified)
          << "\", \"file\": \"" << jsonEscape(r.tests[i].file)
          << "\", \"distance\": " << r.tests[i].distance << "}"
          << (i + 1 < r.tests.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"conflicts\": [\n";
    for (size_t i = 0; i < r.conflicts.size(); ++i) {
        const auto& cp = r.conflicts[i];
        o << "    {\"a\": \"" << jsonEscape(cp.a) << "\", \"b\": \"" << jsonEscape(cp.b)
          << "\", \"sharedCallers\": " << cp.sharedCallers
          << ", \"note\": \"" << jsonEscape(cp.note) << "\"}"
          << (i + 1 < r.conflicts.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    o << "  \"notes\": [";
    for (size_t i = 0; i < r.ambiguityNotes.size(); ++i) {
        if (i) o << ", ";
        o << "\"" << jsonEscape(r.ambiguityNotes[i]) << "\"";
    }
    o << "]\n";
    o << "}\n";
    return o.str();
}

std::string renderImpactDot(const CallGraph& g, const ImpactReport& r, int maxNodes) {
    std::ostringstream o;
    o << "digraph RippleTrace {\n";
    o << "  rankdir=BT;\n";
    o << "  node [shape=box, style=\"rounded,filled\", fontname=\"Consolas\", fontsize=10];\n";
    o << "  edge [color=\"#888888\"];\n";

    std::unordered_set<std::string> changedSet(r.changed.begin(), r.changed.end());
    std::unordered_set<std::string> included;

    int count = 0;
    for (const auto& im : r.impacted) {
        if (count++ >= maxNodes) break;
        included.insert(im.qualified);
    }
    included.insert(changedSet.begin(), changedSet.end());

    auto nodeAttrs = [&](const std::string& id) {
        std::string label = id;
        // escape quotes in label
        std::string clean;
        for (char c : label) { if (c == '"') clean += '\\'; clean += c; }
        if (changedSet.count(id)) {
            return "label=\"" + clean + "\", fillcolor=\"#ffd7d7\", color=\"#cc0000\", penwidth=2";
        }
        const RepoFunction* f = g.find(id);
        if (f && f->isTest) {
            return "label=\"" + clean + "\", fillcolor=\"#d7ecff\", color=\"#0066cc\"";
        }
        if (f && f->isPublicApi) {
            return "label=\"" + clean + "\", fillcolor=\"#fff2cc\", color=\"#cc9900\"";
        }
        return "label=\"" + clean + "\", fillcolor=\"#f2f2f2\"";
    };

    for (const auto& id : included) {
        o << "  \"" << id << "\" [" << nodeAttrs(id) << "];\n";
    }

    // edges that lie on the justification paths, plus direct caller edges
    std::unordered_set<std::string> emitted;
    for (const auto& im : r.impacted) {
        for (size_t i = 0; i + 1 < im.path.size(); ++i) {
            const std::string& caller = im.path[i];
            const std::string& callee = im.path[i + 1];
            if (!included.count(caller) || !included.count(callee)) continue;
            std::string key = caller + "\x01" + callee;
            if (!emitted.insert(key).second) continue;
            o << "  \"" << caller << "\" -> \"" << callee << "\";\n";
        }
    }
    o << "}\n";
    return o.str();
}

std::string renderTestRunList(const ImpactReport& r) {
    std::ostringstream o;
    for (const auto& t : r.tests) o << t.qualified << "\n";
    return o.str();
}

}  // namespace rippletrace
