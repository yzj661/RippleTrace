// ============================================================
//  RippleTrace - diff.h
//  Unified-diff parsing -> changed line ranges per file.
//
//  This is the bridge from "what the developer actually edited" to the
//  graph: it converts a patch into the set of functions whose bodies were
//  touched, so the user does not have to name them by hand.
// ============================================================
#pragma once

#include <string>
#include <vector>

#include "graph.h"

namespace rippletrace {

struct FileHunk {
    std::string path;                 // new-side path, repo relative
    std::vector<std::pair<int, int>> addedLineRanges;  // 1-based, inclusive, new file
    // The same added runs expressed in OLD-file coordinates. Attribution must use
    // these: RippleTrace analyses the current working tree, which is normally the
    // pre-patch revision, so a line number taken from the patch's new side points
    // at the wrong place (an insertion above a function shifts it down).
    std::vector<std::pair<int, int>> addedLineRangesOldSide;
    std::vector<std::pair<int, int>> removedLineRanges;// old-file side, informational
    int addedLines = 0;
    int removedLines = 0;
};

struct DiffParseResult {
    std::vector<FileHunk> files;
    std::vector<std::string> warnings;
    int totalAdded = 0;
    int totalRemoved = 0;
};

// Parse a unified diff (git diff / diff -u). Never throws; malformed
// input is reported through warnings.
DiffParseResult parseUnifiedDiff(const std::string& text);

// Map changed new-file line ranges onto the functions that contain them.
// A function is "touched" when any added line falls inside its body.
struct TouchedFunction {
    std::string qualified;
    std::string file;
    int bodyStart = 0;
    int bodyEnd = 0;
    int addedLinesInBody = 0;
};

std::vector<TouchedFunction> functionsTouchedByDiff(const CallGraph& graph,
                                                    const DiffParseResult& diff);

}  // namespace rippletrace
