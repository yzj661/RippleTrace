// ============================================================
//  RippleTrace - cli_utf8.cpp
// ============================================================
#include "cli_utf8.h"

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#  pragma comment(lib, "shell32.lib")
#endif

namespace rippletrace {

namespace {
std::vector<std::string> g_fallback;
}

void setFallbackArgs(int argc, char** argv) {
    g_fallback.clear();
    for (int i = 0; i < argc; ++i) {
        g_fallback.push_back(argv[i] ? std::string(argv[i]) : std::string());
    }
}

std::vector<std::string> utf8Args() {
#if defined(_WIN32)
    std::vector<std::string> out;
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wargv == nullptr) return g_fallback;

    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::wstring w(wargv[i]);
        if (w.empty()) { out.push_back(std::string()); continue; }
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) { out.push_back(std::string()); continue; }
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            &s[0], n, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    LocalFree(wargv);
    return out;
#else
    return g_fallback;
#endif
}

}  // namespace rippletrace
