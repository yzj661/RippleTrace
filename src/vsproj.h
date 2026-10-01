// ============================================================
//  RippleTrace - vsproj.h
//  Visual Studio project awareness.
//
//  Why this exists, measured on a real project: a folder contained
//  main.cpp (363 lines, compiled) and main(1).cpp (243 lines, a stray copy).
//  Six functions existed in both. RippleTrace scanned both, so a report about
//  gotoXY pointed the engineer at main(1).cpp:73 - a file the build never
//  compiles. Analysing dead code as if it were live is worse than not
//  analysing it, because the answer looks authoritative.
//
//  A .vcxproj lists exactly the files the build uses, so when one is present
//  it should decide the file set instead of "every source file I can find".
// ============================================================
#pragma once

#include <set>
#include <string>
#include <vector>

namespace rippletrace {

struct VsProject {
    std::string path;                       // the .vcxproj that was read
    std::string projectDir;                 // directory containing it
    std::vector<std::string> compile;        // repo-relative .cpp files
    std::vector<std::string> include;        // repo-relative headers
    std::vector<std::string> excluded;       // files on disk but NOT compiled
};

// Find .vcxproj files under `root` (recursively, skipping build dirs).
std::vector<std::string> findVsProjects(const std::string& root);

// Parse one .vcxproj. Returns false when the file cannot be read.
// Paths inside a .vcxproj are relative to the project directory and use
// backslashes; they are normalised to forward slashes and made repo-relative
// to `repoRoot`.
bool parseVsProject(const std::string& projectPath,
                    const std::string& repoRoot,
                    VsProject& out);

}  // namespace rippletrace
