// ============================================================
//  RippleTrace - diff.cpp
// ============================================================
#include "diff.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace rippletrace {

namespace {

std::string normalisePath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    // strip a/ b/ prefixes used by git
    if (p.size() > 2 && (p[0] == 'a' || p[0] == 'b') && p[1] == '/') p.erase(0, 2);
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    return p;
}

// Parse "@@ -a,b +c,d @@" -> oldStart,newStart
bool parseHunkHeader(const std::string& line, int& oldStart, int& newStart) {
    size_t at = line.find("@@");
    if (at == std::string::npos) return false;
    size_t end = line.find("@@", at + 2);
    if (end == std::string::npos) return false;
    std::string body = line.substr(at + 2, end - at - 2);

    size_t minus = body.find('-');
    size_t plus = body.find('+');
    if (minus == std::string::npos || plus == std::string::npos) return false;

    try {
        std::string olds = body.substr(minus + 1, plus - minus - 1);
        std::string news = body.substr(plus + 1);
        size_t comma;
        comma = olds.find(',');
        if (comma != std::string::npos) olds = olds.substr(0, comma);
        comma = news.find(',');
        if (comma != std::string::npos) news = news.substr(0, comma);
        oldStart = std::stoi(olds);
        newStart = std::stoi(news);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

DiffParseResult parseUnifiedDiff(const std::string& text) {
    DiffParseResult res;
    std::istringstream in(text);
    std::string line;

    FileHunk* cur = nullptr;
    int newLine = 0;
    int oldLine = 0;
    bool inHunk = false;

    // Two-phase parsing.
    //
    // Phase 1 collects the exact added-line runs of each hunk, keeping a separate
    // run for every contiguous '+' group, expressed in BOTH new-side and old-side
    // coordinates. Phase 2 publishes them per file.
    //
    // Old-side coordinates are the ones attribution uses. RippleTrace analyses the
    // working tree, which is normally the pre-patch revision, so a new-side line
    // number from the patch points at the wrong line whenever the hunk inserted
    // anything above it.
    struct PendingHunk {
        int newStart = 0;
        std::vector<std::pair<int, int>> addedRuns;         // new side
        std::vector<std::pair<int, int>> addedRunsOldSide;  // old side
        std::vector<int> contextLines;
    };

    std::vector<PendingHunk> hunks;
    PendingHunk* curHunk = nullptr;
    int runStart = -1;
    int lastAdded = -1;
    int runOldStart = -1;
    int runOldEnd = -1;
    // Running new->old offset inside the current hunk:
    //   oldLine == newLine - netAdded + netRemoved
    int netAdded = 0;
    int netRemoved = 0;

    auto closeRun = [&]() {
        if (curHunk && runStart >= 0 && lastAdded >= runStart) {
            curHunk->addedRuns.emplace_back(runStart, lastAdded);
            curHunk->addedRunsOldSide.emplace_back(runOldStart, runOldEnd);
        }
        runStart = -1;
        lastAdded = -1;
        runOldStart = -1;
        runOldEnd = -1;
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("diff --git", 0) == 0) {
            closeRun();
            size_t bpos = line.find(" b/");
            std::string p;
            if (bpos != std::string::npos) p = line.substr(bpos + 3);
            res.files.push_back(FileHunk{});
            res.files.back().path = normalisePath(p);
            cur = &res.files.back();
            curHunk = nullptr;
            inHunk = false;
            continue;
        }
        if (line.rfind("+++ ", 0) == 0) {
            std::string p = line.substr(4);
            size_t tab = p.find('\t');
            if (tab != std::string::npos) p = p.substr(0, tab);
            if (p == "/dev/null") continue;
            if (!cur) { res.files.push_back(FileHunk{}); cur = &res.files.back(); }
            cur->path = normalisePath(p);
            continue;
        }
        if (line.rfind("--- ", 0) == 0) continue;

        if (line.rfind("@@", 0) == 0) {
            closeRun();
            int o = 0, nn = 0;
            if (!parseHunkHeader(line, o, nn)) {
                res.warnings.push_back("unparsable hunk header: " + line);
                inHunk = false;
                curHunk = nullptr;
                continue;
            }
            oldLine = o;
            newLine = nn;
            netAdded = 0;
            netRemoved = 0;
            inHunk = (cur != nullptr);
            if (inHunk) {
                hunks.push_back(PendingHunk{});
                curHunk = &hunks.back();
                curHunk->newStart = nn;
            }
            continue;
        }

        if (!inHunk || !cur || !curHunk) continue;

        if (!line.empty() && line[0] == '+') {
            if (runStart < 0) runStart = newLine;
            lastAdded = newLine;
            const int oldPos = newLine - netAdded + netRemoved;
            if (runOldStart < 0) runOldStart = oldPos;
            runOldEnd = oldPos;
            ++netAdded;
            ++newLine;
            ++cur->addedLines;
            ++res.totalAdded;
        } else if (!line.empty() && line[0] == '-') {
            ++netRemoved;
            ++oldLine;
            ++cur->removedLines;
            ++res.totalRemoved;
            if (cur->removedLineRanges.empty() ||
                oldLine - 1 > cur->removedLineRanges.back().second + 1) {
                cur->removedLineRanges.emplace_back(oldLine - 1, oldLine - 1);
            } else {
                cur->removedLineRanges.back().second = oldLine - 1;
            }
        } else if (line.empty() || line[0] == ' ') {
            // A context line is either " <text>" or -- for a blank source line --
            // the single character "\n", which std::getline yields as an empty
            // string. Treating the empty case as malformed used to abort the hunk
            // early, silently dropping every later addition in it.
            closeRun();
            curHunk->contextLines.push_back(newLine);
            ++newLine;
            ++oldLine;
        } else if (line.rfind("\\ No newline", 0) == 0) {
            // ignore
        } else {
            closeRun();
            inHunk = false;
        }
    }
    closeRun();

    // ---- phase 2: extend each added run over adjacent context lines ----
    {
        std::unordered_map<std::string, std::vector<const PendingHunk*>> byPath;
        for (const auto& h : hunks) byPath[cur ? cur->path : std::string()].push_back(&h);
        (void)byPath;

        size_t hunkIdx = 0;
        for (auto& f : res.files) {
            (void)f;
        }

        // Walk hunks in order; they belong to the file record that was current
        // when they were parsed, which we reconstruct by re-scanning the text.
        std::istringstream in2(text);
        std::string l2;
        FileHunk* file = nullptr;
        size_t hi = 0;
        while (std::getline(in2, l2)) {
            if (!l2.empty() && l2.back() == '\r') l2.pop_back();
            if (l2.rfind("diff --git", 0) == 0) {
                size_t bpos = l2.find(" b/");
                std::string p = (bpos == std::string::npos) ? std::string() : l2.substr(bpos + 3);
                p = normalisePath(p);
                for (auto& ff : res.files) {
                    if (ff.path == p) { file = &ff; break; }
                }
                continue;
            }
            if (l2.rfind("@@", 0) == 0) {
                if (hi >= hunks.size()) break;
                const PendingHunk& h = hunks[hi++];
                if (!file) continue;
                for (size_t r = 0; r < h.addedRuns.size(); ++r) {
                    file->addedLineRanges.push_back(h.addedRuns[r]);
                    if (r < h.addedRunsOldSide.size()) {
                        file->addedLineRangesOldSide.push_back(h.addedRunsOldSide[r]);
                    }
                }
            }
        }
    }

    // drop empty file records
    res.files.erase(std::remove_if(res.files.begin(), res.files.end(),
                                   [](const FileHunk& f) {
                                       return f.path.empty() && f.addedLines == 0 &&
                                              f.removedLines == 0;
                                   }),
                    res.files.end());
    return res;
}

std::vector<TouchedFunction> functionsTouchedByDiff(const CallGraph& graph,
                                                    const DiffParseResult& diff) {
    std::unordered_map<std::string, const FileHunk*> byPath;
    for (const auto& f : diff.files) byPath[f.path] = &f;

    // Group the graph's functions by file so attribution can respect the real
    // function boundaries: an added line just above a signature belongs to that
    // function, but must not be allowed to bleed into the previous one.
    std::unordered_map<std::string, std::vector<const RepoFunction*>> byFile;
    for (const RepoFunction* fn : graph.allFunctions()) {
        byFile[normalisePath(fn->file)].push_back(fn);
    }
    for (auto& kv : byFile) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const RepoFunction* a, const RepoFunction* b) {
                      return a->bodyLineStart < b->bodyLineStart;
                  });
    }

    // How far the effective range may stretch beyond a body, to absorb
    // signature-only and trailing-brace edits without overlapping neighbours.
    const int kLeadIn = 6;
    const int kLeadOut = 2;

    std::vector<TouchedFunction> out;

    for (auto& kv : byFile) {
        const std::string& rel = kv.first;
        auto it = byPath.find(rel);
        if (it == byPath.end()) {
            for (const auto& cand : byPath) {
                const std::string& p = cand.first;
                if (rel.size() >= p.size() &&
                    rel.compare(rel.size() - p.size(), p.size(), p) == 0) {
                    it = byPath.find(p);
                    break;
                }
            }
            if (it == byPath.end()) continue;
        }
        const FileHunk* hunk = it->second;
        const std::vector<const RepoFunction*>& fns = kv.second;

        // Assign every added line to at most ONE function, using OLD-side line
        // numbers because that is the revision the graph was built from.
        //
        // The rule is:
        //   * a line inside a body belongs to that function;
        //   * a line in the gap immediately above a body belongs to the function
        //     starting after it (how a signature edit or an insertion just above a
        //     signature is attributed);
        //   * the gap is split at its midpoint so neighbouring functions can never
        //     claim the same line.
        std::unordered_map<std::string, int> hitsPerFunction;

        const std::vector<std::pair<int, int>>& ranges =
            hunk->addedLineRangesOldSide.empty() ? hunk->addedLineRanges
                                                 : hunk->addedLineRangesOldSide;

        for (const auto& r : ranges) {
            int lo = r.first;
            int hi = r.second;
            if (hi < lo) hi = lo;

            for (int line = lo; line <= hi; ++line) {
                // pick the last function whose tier range contains this line
                const RepoFunction* owner = nullptr;
                for (size_t i = 0; i < fns.size(); ++i) {
                    const RepoFunction* fn = fns[i];

                    int tierStart = fn->bodyLineStart;
                    int tierEnd = fn->bodyLineEnd;

                    // claim part of the gap above this body, up to the midpoint
                    if (i > 0) {
                        int prevEnd = fns[i - 1]->bodyLineEnd;
                        int gap = fn->bodyLineStart - prevEnd - 1;
                        int share = (gap + 1) / 2;
                        tierStart -= std::max(0, std::min(kLeadIn, share));
                    }
                    // claim part of the gap below this body, up to the midpoint
                    if (i + 1 < fns.size()) {
                        int nextStart = fns[i + 1]->bodyLineStart;
                        int gap = nextStart - fn->bodyLineEnd - 1;
                        int share = (gap + 1) / 2;
                        tierEnd += std::max(0, std::min(kLeadOut, share));
                    }

                    if (line >= tierStart && line <= tierEnd) {
                        owner = fn;
                        break;   // tiers do not overlap by construction
                    }
                }
                if (owner != nullptr) hitsPerFunction[owner->qualified] += 1;
            }
        }

        for (const RepoFunction* fn : fns) {
            auto h = hitsPerFunction.find(fn->qualified);
            if (h == hitsPerFunction.end() || h->second <= 0) continue;
            TouchedFunction tf;
            tf.qualified = fn->qualified;
            tf.file = fn->file;
            tf.bodyStart = fn->bodyLineStart;
            tf.bodyEnd = fn->bodyLineEnd;
            tf.addedLinesInBody = h->second;
            out.push_back(std::move(tf));
        }
    }

    std::sort(out.begin(), out.end(), [](const TouchedFunction& a, const TouchedFunction& b) {
        if (a.addedLinesInBody != b.addedLinesInBody) return a.addedLinesInBody > b.addedLinesInBody;
        return a.qualified < b.qualified;
    });
    return out;
}

}  // namespace rippletrace
