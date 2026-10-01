// ============================================================
//  RippleTrace - lang.h
//  Language descriptions as data.
//
//  Background: the first version hard-coded C++ everywhere - the scope
//  separator "::" appeared in 12 places, the keyword table and the extension
//  list were literals. That is fine for one language and unworkable for
//  three, so every language-specific fact now lives in a LanguageProfile.
//
//  What is genuinely shared across languages (and therefore NOT in here):
//    * the call graph, reverse reachability, risk scoring
//    * test selection, change-interaction detection
//    * diff-to-function attribution, the AI grounding layer
//  Those operate on (function, call edge) facts and never look at syntax.
// ============================================================
#pragma once

#include <set>
#include <string>
#include <vector>

namespace rippletrace {

// How a language delimits blocks. This decides which extraction engine runs.
enum class ScopeStyle {
    Brace,     // C, C++, Java, C#, Go, Rust:  { }
    Indent     // Python: significant indentation
};

// How callable units are written. Used for reporting and for heuristics.
enum class DefStyle {
    CppFunction,   // returnType name(params) { }        (C, C++)
    ClassMethod,   // modifiers returnType name(params) { }  (Java)
    PyDef          // def name(params):                  (Python)
};

struct LanguageProfile {
    std::string id;                       // "cpp", "java", "python"
    std::string displayName;              // "C++"
    std::vector<std::string> extensions;  // ".cpp", ".java", ".py"
    std::string scopeSeparator;           // "::" or "."
    ScopeStyle scopeStyle = ScopeStyle::Brace;
    DefStyle defStyle = DefStyle::CppFunction;

    // Keywords that can never be a function name in a definition.
    std::set<std::string> definitionKeywords;
    // Keywords that can never be a call target.
    std::set<std::string> nonCallKeywords;
    // Constructs that open a block but are not a function body.
    std::set<std::string> blockIntroducers;

    // Comment syntax.
    std::string lineComment;              // "//" or "#"
    bool hasBlockComment = true;          // /* */ present?

    // Marker used in prompts and reports. "//" for C-family, "#" for Python.
    std::string annotationPrefix;

    // How well calls can be resolved in this language. Reported to the user so
    // that a low-confidence result is never presented as a confident one.
    std::string resolutionNote;
};

// Built-in profiles.
const LanguageProfile& cppProfile();
const LanguageProfile& javaProfile();
const LanguageProfile& pythonProfile();

// Every profile the binary knows about.
std::vector<const LanguageProfile*> allProfiles();

// Identify a language from a file path. Returns nullptr when unknown.
const LanguageProfile* profileForPath(const std::string& path);

// Identify a language by id ("cpp", "java", "python").
const LanguageProfile* profileById(const std::string& id);

// True when the path looks like a header rather than a source file.
bool isHeaderPathFor(const LanguageProfile& profile, const std::string& path);

}  // namespace rippletrace
