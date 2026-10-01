// ============================================================
//  RippleTrace - lang.cpp
//  The built-in language descriptions.
// ============================================================
#include "lang.h"

#include <algorithm>
#include <cctype>

namespace rippletrace {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

const LanguageProfile& cppProfile() {
    static const LanguageProfile p = [] {
        LanguageProfile lp;
        lp.id = "cpp";
        lp.displayName = "C++";
        lp.extensions = {".cpp", ".cc", ".cxx", ".c++", ".hpp", ".hh", ".hxx",
                         ".h",   ".h++", ".inl", ".ipp", ".tpp"};
        lp.scopeSeparator = "::";
        lp.scopeStyle = ScopeStyle::Brace;
        lp.defStyle = DefStyle::CppFunction;
        lp.lineComment = "//";
        lp.hasBlockComment = true;
        lp.annotationPrefix = "//";

        lp.definitionKeywords = {
            "if", "for", "while", "switch", "catch", "do", "else", "try",
            "namespace", "class", "struct", "union", "enum", "extern",
            "sizeof", "alignof", "decltype", "return", "new", "delete",
            "throw", "static_assert", "operator", "typedef", "using",
            "template", "typename", "concept", "requires", "co_await",
            "co_return", "co_yield", "public", "private", "protected",
            "virtual", "explicit", "friend", "inline", "constexpr",
            "consteval", "constinit", "mutable", "register", "thread_local",
        };
        lp.nonCallKeywords = {
            "if", "for", "while", "switch", "catch", "return", "sizeof",
            "alignof", "decltype", "noexcept", "static_assert", "do", "else",
            "case", "new", "delete", "throw", "and", "or", "not", "assert",
        };
        lp.blockIntroducers = {
            "if", "for", "while", "switch", "catch", "do", "else", "try",
            "namespace", "class", "struct", "union", "enum", "extern",
        };

        lp.resolutionNote =
            "C++ resolves a call by walking the enclosing scope chain; "
            "templates, overloads and virtual dispatch stay unresolved.";
        return lp;
    }();
    return p;
}

const LanguageProfile& javaProfile() {
    static const LanguageProfile p = [] {
        LanguageProfile lp;
        lp.id = "java";
        lp.displayName = "Java";
        lp.extensions = {".java"};
        lp.scopeSeparator = ".";
        lp.scopeStyle = ScopeStyle::Brace;
        lp.defStyle = DefStyle::ClassMethod;
        lp.lineComment = "//";
        lp.hasBlockComment = true;
        lp.annotationPrefix = "//";

        lp.definitionKeywords = {
            // control flow - never a method declarator
            "if", "for", "while", "switch", "catch", "do", "else", "try",
            "finally", "return", "throw", "synchronized", "assert",
            // type / member introducers
            "class", "interface", "enum", "record", "new", "static", "final",
            "abstract", "native", "transient", "volatile", "package",
            "import", "extends", "implements", "throws", "instanceof",
            "this", "super", "boolean", "byte", "char", "short", "int",
            "long", "float", "double", "void", "var",
        };
        lp.nonCallKeywords = {
            "if", "for", "while", "switch", "catch", "return", "synchronized",
            "assert", "new", "throw", "instanceof", "super", "this", "case",
            "do", "else", "try",
        };
        lp.blockIntroducers = {
            "if", "for", "while", "switch", "catch", "do", "else", "try",
            "finally", "synchronized", "class", "interface", "enum", "record",
            "static", "new",
        };

        lp.resolutionNote =
            "Java has no project-wide import graph here, so a call like "
            "service.find(id) cannot be tied to a class; only same-file and "
            "uniquely-named project methods resolve.";
        return lp;
    }();
    return p;
}

const LanguageProfile& pythonProfile() {
    static const LanguageProfile p = [] {
        LanguageProfile lp;
        lp.id = "python";
        lp.displayName = "Python";
        lp.extensions = {".py", ".pyw"};
        lp.scopeSeparator = ".";
        lp.scopeStyle = ScopeStyle::Indent;
        lp.defStyle = DefStyle::PyDef;
        lp.lineComment = "#";
        lp.hasBlockComment = false;
        lp.annotationPrefix = "#";

        lp.definitionKeywords = {
            "if", "elif", "else", "for", "while", "with", "try", "except",
            "finally", "return", "yield", "raise", "assert", "import",
            "from", "pass", "break", "continue", "global", "nonlocal",
            "del", "lambda", "await", "async", "match", "case",
            "and", "or", "not", "in", "is", "as", "class",
        };
        lp.nonCallKeywords = {
            "if", "elif", "for", "while", "with", "return", "yield", "raise",
            "assert", "import", "from", "print", "not", "and", "or", "in",
            "is", "del", "await", "lambda", "class", "def", "except", "exec",
        };
        lp.blockIntroducers = {
            "if", "elif", "else", "for", "while", "with", "try", "except",
            "finally", "class", "match", "case",
        };

        lp.resolutionNote =
            "Python dispatch is dynamic: obj.method() carries no static type, "
            "so method calls resolve only when the name is unique in the repo "
            "or an explicit self.method() style call is recognised.";
        return lp;
    }();
    return p;
}

std::vector<const LanguageProfile*> allProfiles() {
    return {&cppProfile(), &javaProfile(), &pythonProfile()};
}

const LanguageProfile* profileForPath(const std::string& path) {
    const std::string p = lower(path);
    for (const LanguageProfile* lp : allProfiles()) {
        for (const auto& ext : lp->extensions) {
            if (endsWith(p, ext)) return lp;
        }
    }
    return nullptr;
}

const LanguageProfile* profileById(const std::string& id) {
    const std::string want = lower(id);
    for (const LanguageProfile* lp : allProfiles()) {
        if (lp->id == want) return lp;
    }
    return nullptr;
}

bool isHeaderPathFor(const LanguageProfile& profile, const std::string& path) {
    const std::string p = lower(path);
    if (profile.id == "cpp") {
        const char* hdr[] = {".hpp", ".hh", ".hxx", ".h", ".h++", ".inl", ".ipp", ".tpp"};
        for (const char* h : hdr) {
            if (endsWith(p, h)) return true;
        }
        return false;
    }
    // Java and Python have no separate header concept.
    return false;
}

}  // namespace rippletrace
