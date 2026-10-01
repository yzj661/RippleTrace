// ============================================================
//  RippleTrace - repo.h
//  Repository scanning: file discovery and source loading.
// ============================================================
#pragma once

#include <string>
#include <filesystem>
#include <vector>

#include "lang.h"

namespace rippletrace {

struct SourceFile {
    std::string absolutePath;
    std::string repoRelativePath;
    bool isHeader = false;
    const LanguageProfile* language = nullptr;
    // True when a Visual Studio project was found and it does NOT list this
    // file. Such a file is dead code: it sits on disk but the build never
    // compiles it. Reporting impact against it would send the engineer to the
    // wrong file, so it is excluded unless explicitly requested.
    bool notInBuild = false;
};

struct ScanOptions {
    // When empty, every language known to allProfiles() is scanned.
    std::vector<std::string> languages;
    std::vector<std::string> excludeDirs = {
        ".git", "build", "out", "node_modules", ".vs",
        "cmake-build-debug", "cmake-build-release",
        "__pycache__", "target", ".idea", "venv", ".venv", "dist",
    };
    size_t maxFileBytes = 4u * 1024u * 1024u;
    // When a .vcxproj is present, restrict the scan to the files it compiles.
    // Set false to analyse every source file on disk.
    bool honourVsProjects = true;
    // Include files that exist on disk but are not in any build.
    bool includeNonBuildFiles = false;
};

// What the scan decided, for reporting.
struct ScanSummary {
    bool vsProjectFound = false;
    std::string vsProjectPath;
    size_t fromBuild = 0;         // files the build actually compiles
    size_t excludedNotInBuild = 0;
};

// Recursively collect source files under `root`.
std::vector<SourceFile> scanRepository(const std::string& root,
                                       const ScanOptions& opt = ScanOptions());

// As above, but also reports what the scan decided about build membership.
std::vector<SourceFile> scanRepository(const std::string& root,
                                       const ScanOptions& opt,
                                       ScanSummary& summary);

// Read a text file; returns false on failure.
bool readFileText(const std::string& path, std::string& out);

// ------------------------------------------------------------
//  UTF-8 path helpers.
//
//  std::filesystem::path cannot be built from a UTF-8 std::string on Windows:
//  the constructor decodes using the ANSI code page, so any path with Chinese
//  characters throws "No mapping for the Unicode character exists in the
//  target multi-byte code page". Every path that arrives from a URL, a JSON
//  body or the command line is UTF-8, so it must go through these.
// ------------------------------------------------------------
std::filesystem::path utf8ToPath(const std::string& utf8);
std::string pathToUtf8(const std::filesystem::path& p);

// Join paths with the platform separator, normalising separators in `b`.
std::string joinPath(const std::string& a, const std::string& b);

// Repo-relative path using forward slashes.
std::string toRepoRelative(const std::string& file, const std::string& root);

}  // namespace rippletrace
