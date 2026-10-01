// ============================================================
//  RippleTrace - vsproj.cpp
// ============================================================
#include "vsproj.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lang.h"
#include "repo.h"

namespace rippletrace {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return lower(s).compare(s.size() - suffix.size(), suffix.size(), lower(suffix)) == 0;
}

// .vcxproj is XML. Rather than pull in a parser, extract the two element kinds
// we care about with a small scan - the shape is rigid enough for that, and any
// miss simply falls back to scanning the directory.
void collectIncludes(const std::string& xml,
                     std::vector<std::string>& compile,
                     std::vector<std::string>& include) {
    const char* tags[] = {"ClCompile", "ClInclude"};
    for (const char* tag : tags) {
        std::string open = std::string("<") + tag + " Include=\"";
        size_t pos = 0;
        while ((pos = xml.find(open, pos)) != std::string::npos) {
            size_t start = pos + open.size();
            size_t end = xml.find('"', start);
            if (end == std::string::npos) break;
            std::string path = xml.substr(start, end - start);
            std::replace(path.begin(), path.end(), '\\', '/');
            if (std::string(tag) == "ClCompile") compile.push_back(path);
            else include.push_back(path);
            pos = end;
        }
    }
}

std::string readAll(const std::string& path) {
    std::ifstream in(utf8ToPath(path), std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

// Make `rel` (relative to projectDir) relative to repoRoot.
std::string toRepoRel(const std::string& repoRoot, const std::string& projectDir,
                      const std::string& rel) {
    std::error_code ec;
    fs::path full = utf8ToPath(projectDir) / rel;
    fs::path root = utf8ToPath(repoRoot);
    fs::path r = fs::relative(full, root, ec);
    if (ec) return rel;
    return pathToUtf8(r);
}

}  // namespace

std::vector<std::string> findVsProjects(const std::string& root) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(utf8ToPath(root), ec)) return out;

    static const std::vector<std::string> skip = {
        ".git", "build", "out", ".vs", "x64", "Debug", "Release",
        "node_modules", "__pycache__", "target",
    };
    fs::recursive_directory_iterator it(
        utf8ToPath(root), fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::directory_entry& de = *it;
        std::error_code fec;
        if (de.is_directory(fec)) {
            const std::string name = pathToUtf8(de.path().filename());
            if (std::find(skip.begin(), skip.end(), name) != skip.end()) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!de.is_regular_file(fec)) continue;
        if (endsWithCI(pathToUtf8(de.path()), ".vcxproj")) {
            out.push_back(pathToUtf8(de.path()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool parseVsProject(const std::string& projectPath,
                    const std::string& repoRoot,
                    VsProject& out) {
    const std::string xml = readAll(projectPath);
    if (xml.empty()) return false;

    out.path = projectPath;
    out.projectDir = pathToUtf8(utf8ToPath(projectPath).parent_path());

    std::vector<std::string> rawCompile, rawInclude;
    collectIncludes(xml, rawCompile, rawInclude);

    for (const auto& c : rawCompile) {
        out.compile.push_back(toRepoRel(repoRoot, out.projectDir, c));
    }
    for (const auto& c : rawInclude) {
        out.include.push_back(toRepoRel(repoRoot, out.projectDir, c));
    }

    std::sort(out.compile.begin(), out.compile.end());
    out.compile.erase(std::unique(out.compile.begin(), out.compile.end()), out.compile.end());
    std::sort(out.include.begin(), out.include.end());
    out.include.erase(std::unique(out.include.begin(), out.include.end()), out.include.end());
    return !out.compile.empty() || !out.include.empty();
}

}  // namespace rippletrace
