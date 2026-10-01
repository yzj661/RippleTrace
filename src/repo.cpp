// ============================================================
//  RippleTrace - repo.cpp
// ============================================================
#include "repo.h"
#include "utf8.h"
#include "vsproj.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace rippletrace {

namespace fs = std::filesystem;

namespace {

// Key used to match a file on disk against a build-file list: forward slashes
// and lower case, so a .vcxproj written with backslashes and mixed case still
// matches, and a leading "./" does not defeat the comparison.
std::string normaliseKey(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (s.size() > 1 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

bool languageAllowed(const LanguageProfile& lp, const std::vector<std::string>& wanted) {
    if (wanted.empty()) return true;
    for (const auto& w : wanted) {
        if (lp.id == w || lp.displayName == w) return true;
    }
    return false;
}

bool excluded(const fs::path& p, const std::vector<std::string>& dirs) {
    for (const auto& d : dirs) {
        if (pathToUtf8(p.filename()) == d) return true;
    }
    return false;
}

}  // namespace

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::string out = a;
    char last = out.back();
    if (last != '/' && last != '\\') out.push_back('/');
    std::string bb = b;
    std::replace(bb.begin(), bb.end(), '\\', '/');
    while (!bb.empty() && bb.front() == '/') bb.erase(bb.begin());
    out += bb;
    return out;
}

std::string toRepoRelative(const std::string& file, const std::string& root) {
    std::error_code ec;
    fs::path f = fs::weakly_canonical(utf8ToPath(file), ec);
    if (ec) f = fs::absolute(utf8ToPath(file), ec);
    fs::path r = fs::weakly_canonical(utf8ToPath(root), ec);
    if (ec) r = fs::absolute(utf8ToPath(root), ec);

    fs::path rel;
    std::error_code ec2;
    rel = fs::relative(f, r, ec2);
    std::string s;
    if (ec2 || rel.empty()) {
        s = pathToUtf8(f.filename());
    } else {
        s = pathToUtf8(rel);
    }
    return s;
}

namespace {

// Strict UTF-8 validation lives in utf8.h, shared with the JSON encoder. It is
// deliberately stricter than "the continuation bytes are in range": encoded
// surrogates, overlong forms and code points past U+10FFFF are all rejected.
bool isValidUtf8(const std::string& s) {
    return utf8::isWellFormed(s);
}

#if defined(_WIN32)
// Decode a legacy single/double byte code page into UTF-8.
bool decodeCodePage(const std::string& raw, unsigned codePage, std::string& out) {
    if (raw.empty()) { out.clear(); return true; }
    int wideLen = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, raw.c_str(),
                                      static_cast<int>(raw.size()), nullptr, 0);
    if (wideLen <= 0) return false;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, raw.c_str(),
                            static_cast<int>(raw.size()), &wide[0], wideLen) <= 0) {
        return false;
    }
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                      static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) return false;
    out.assign(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &out[0], utf8Len, nullptr, nullptr);
    return true;
}

// Source files are not always UTF-8. A Visual Studio project on a Chinese
// Windows install commonly mixes encodings: some files UTF-8, others GBK.
//
// Two separate hazards live here, and both end in the same place:
//
//   * GBK bytes often form a *well-formed* UTF-8 sequence, so a lenient
//     "is this valid UTF-8?" test accepts them; and
//   * a code page decode can still yield an unpaired UTF-16 surrogate, which
//     the Windows API then writes back out as WTF-8 (ED A0 80).
//
// Either way the surrogate reaches the model prompt and the endpoint rejects
// the body: "HTTP 400 ... lone leading surrogate in hex escape". So the decode
// result is sanitised rather than trusted, and the reader is guaranteed to hand
// out well-formed UTF-8.
//
// Deeper analysis (identifiers and calls) is byte-oriented and unaffected, but
// every string that can reach a prompt or a report must be valid UTF-8, so the
// conversion belongs here, at the point of reading.
bool toUtf8Text(const std::string& raw, std::string& out) {
    if (isValidUtf8(raw)) { out = raw; return true; }

    // Prefer the ANSI code page the file was most likely written in. A decoder
    // that succeeds may still contain a stray unpaired surrogate, so the output
    // goes through sanitize(); the readable text survives and only the bad code
    // point becomes U+FFFD.
    std::string decoded;
    if (decodeCodePage(raw, CP_ACP, decoded) ||
        decodeCodePage(raw, 936 /* GBK */, decoded) ||
        decodeCodePage(raw, 1252, decoded)) {
        out = utf8::sanitize(decoded);
        return true;
    }

    // No code page claimed it. Replace just the unrepresentable bytes so the
    // readable parts of the file survive.
    out = utf8::sanitize(raw);
    return true;
}
#else
bool toUtf8Text(const std::string& raw, std::string& out) {
    // No code page conversion off Windows, but the same guarantee: the reader
    // never hands out a string that is not well-formed UTF-8.
    out = utf8::sanitize(raw);
    return true;
}
#endif

}  // namespace

bool readFileText(const std::string& path, std::string& out) {
    std::ifstream in(utf8ToPath(path), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string raw = ss.str();

    // strip UTF-8 BOM
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    return toUtf8Text(raw, out);
}


// ------------------------------------------------------------
//  UTF-8 <-> filesystem::path
// ------------------------------------------------------------
#if defined(_WIN32)
std::filesystem::path utf8ToPath(const std::string& utf8) {
    if (utf8.empty()) return std::filesystem::path();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return std::filesystem::path(utf8);   // fall back
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &wide[0], n);
    return std::filesystem::path(wide);
}

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::wstring wide = p.wstring();
    if (wide.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                static_cast<int>(wide.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}
#else
std::filesystem::path utf8ToPath(const std::string& utf8) {
    return std::filesystem::path(utf8);
}
std::string pathToUtf8(const std::filesystem::path& p) { return p.string(); }
#endif

std::vector<SourceFile> scanRepository(const std::string& root, const ScanOptions& opt) {
    ScanSummary ignored;
    return scanRepository(root, opt, ignored);
}

std::vector<SourceFile> scanRepository(const std::string& root, const ScanOptions& opt,
                                       ScanSummary& summary) {
    std::vector<SourceFile> files;
    std::error_code ec;
    if (!fs::exists(utf8ToPath(root), ec)) return files;

    // A Visual Studio project, when present, is the authoritative list of what
    // the build compiles. Without this the scan picks up stray copies such as
    // "main(1).cpp" and reports impact against code that is never built.
    std::set<std::string> buildFiles;   // normalised, repo-relative
    if (opt.honourVsProjects) {
        std::vector<std::string> projects = findVsProjects(root);
        if (!projects.empty()) {
            summary.vsProjectFound = true;
            summary.vsProjectPath = projects.front();
            for (const std::string& proj : projects) {
                VsProject vp;
                if (!parseVsProject(proj, root, vp)) continue;
                for (const auto& c : vp.compile) buildFiles.insert(normaliseKey(c));
                for (const auto& c : vp.include) buildFiles.insert(normaliseKey(c));
            }
        }
    }
    fs::recursive_directory_iterator it(
        utf8ToPath(root), fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::directory_entry& de = *it;
        std::error_code fec;
        if (de.is_directory(fec)) {
            if (excluded(de.path(), opt.excludeDirs)) it.disable_recursion_pending();
            continue;
        }
        if (!de.is_regular_file(fec)) continue;
        std::string p = pathToUtf8(de.path());

        // Language is decided by extension through the profile table, so
        // adding a language is a data change rather than a code change.
        const LanguageProfile* lang = profileForPath(p);
        if (lang == nullptr) continue;
        if (!languageAllowed(*lang, opt.languages)) continue;

        std::uintmax_t sz = de.file_size(fec);
        if (fec || sz == 0 || sz > opt.maxFileBytes) continue;

        SourceFile sf;
        sf.absolutePath = p;
        sf.repoRelativePath = toRepoRelative(p, root);
        sf.language = lang;
        sf.isHeader = isHeaderPathFor(*lang, p);

        if (!buildFiles.empty()) {
            if (buildFiles.count(normaliseKey(sf.repoRelativePath)) > 0) {
                ++summary.fromBuild;
            } else {
                sf.notInBuild = true;
                ++summary.excludedNotInBuild;
                if (!opt.includeNonBuildFiles) continue;
            }
        }

        files.push_back(std::move(sf));
    }

    std::sort(files.begin(), files.end(), [](const SourceFile& a, const SourceFile& b) {
        return a.repoRelativePath < b.repoRelativePath;
    });
    return files;
}

}  // namespace rippletrace
