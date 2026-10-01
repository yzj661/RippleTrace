// ============================================================
//  RippleTrace - graph.cpp
// ============================================================
#include "graph.h"
#include "lang.h"

#include <algorithm>
#include <set>
#include <unordered_set>

namespace rippletrace {

namespace {

// Candidate qualified names to try for a callee written as `written`
// inside `caller`. Ordered most-specific first.
//
// The scope chain is built from whichever separator the language uses, and
// resolution deliberately tries the other one too. That costs nothing and it
// makes the resolver tolerant of a caller whose language we cannot see (for
// example a Java call reaching a C++ target through FFI), which is exactly the
// kind of case that used to be silently dropped when "::" was hard-coded.
std::vector<std::string> candidateNames(const std::string& caller,
                                        const std::string& written,
                                        const std::string& separator) {
    std::vector<std::string> out;
    out.push_back(written);

    const bool dotStyle = (separator == ".");
    const std::string other = dotStyle ? "::" : ".";

    auto separators = [&]() {
        return std::vector<std::string>{separator, other};
    };

    // Scope chain of the caller: A::B::c -> {A::B, A}  (and the dotted form)
    std::vector<std::string> scopes;
    for (const std::string& sep : separators()) {
        std::string s = caller;
        while (true) {
            size_t p = s.rfind(sep);
            if (p == std::string::npos) break;
            s = s.substr(0, p);
            if (!s.empty()) scopes.push_back(s);
        }
    }

    // `written` may itself be partly qualified.
    size_t wq = written.rfind(other);
    size_t wq2 = written.rfind(separator);
    if (wq2 != std::string::npos && (wq == std::string::npos || wq2 > wq)) wq = wq2;
    std::string wScope = (wq == std::string::npos) ? std::string() : written.substr(0, wq);
    std::string wName = (wq == std::string::npos) ? written : written.substr(wq + 1);

    for (const std::string& sc : scopes) {
        for (const std::string& sep : separators()) {
            if (wScope.empty()) {
                out.push_back(sc + sep + wName);
            } else {
                out.push_back(sc + sep + written);
            }
        }
    }
    // global fallbacks
    if (!wScope.empty()) out.push_back(wName);
    return out;
}

// Pull the trailing identifier out of a qualified/dotted name.
std::string tailIdent(const std::string& id) {
    size_t a = id.rfind("::");
    size_t b = id.rfind('.');
    size_t p = std::max(a == std::string::npos ? 0 : a + 2,
                        b == std::string::npos ? 0 : b + 1);
    return (p == 0) ? id : id.substr(p);
}

}  // namespace

void CallGraph::addParse(const ParseResult& pr) {
    // Renamed functions, so their edges can be rewritten to match.
    std::map<std::string, std::string> renames;

    for (const auto& f : pr.functions) {
        if (!f.isDefinition) continue;

        RepoFunction fn = f;
        const LanguageProfile* lp = profileForPath(fn.file);
        const std::string lang = lp ? lp->id : std::string();

        // A qualified name is only unique WITHIN a language. "main" exists in
        // C++, Java and Python, and a plain map keyed on the qualified name kept
        // whichever file happened to be merged first and silently dropped the
        // rest - so a real function vanished from the graph and any impact chain
        // through it broke. When a name is already taken by a different
        // language, suffix the newcomer so both survive.
        auto clash = [&](const std::string& id) -> bool {
            auto it = funcs_.find(id);
            if (it == funcs_.end()) return false;
            const LanguageProfile* other = profileForPath(it->second.file);
            return other != nullptr && lp != nullptr && other->id != lang;
        };
        if (clash(fn.qualified) && !lang.empty()) {
            fn.qualified = fn.qualified + "@" + lang;
            warnings_.push_back("name collision across languages: " + f.qualified +
                                " is defined in more than one language; the one in " +
                                (lp ? lp->displayName : lang) + " is tracked as '" +
                                fn.qualified + "'");
        }

        auto it = funcs_.find(fn.qualified);
        if (it == funcs_.end()) {
            funcs_.emplace(fn.qualified, fn);
            byShortName_[fn.name].push_back(fn.qualified);
            if (fn.qualified != f.qualified) {
                // Record the rename so the edges below can follow it.
                renames[f.qualified] = fn.qualified;
            }
        } else {
            // Same language, same qualified name, different file: a genuine
            // duplicate. First definition wins and the clash is reported.
            if (it->second.file != fn.file) {
                warnings_.push_back("duplicate definition: " + fn.qualified + " (" +
                                    it->second.file + " vs " + fn.file + ")");
            }
        }
    }
    // Edges are resolved after all units are loaded, so buffer them.
    //
    // Any edge whose caller was renamed for a cross-language collision must be
    // rewritten too. Without this the node exists as "main@python" while its own
    // edges still say "main", so the two never join and every call out of that
    // function is lost - which is exactly the symptom that made an impact chain
    // stop one hop short of main().
    for (CallEdge e : pr.edges) {
        if (!renames.empty()) {
            auto r1 = renames.find(e.caller);
            if (r1 != renames.end()) e.caller = r1->second;
            auto r2 = renames.find(e.calleeQualified);
            if (r2 != renames.end()) e.calleeQualified = r2->second;
        }
        pending_.push_back(std::move(e));
    }
    warnings_.insert(warnings_.end(), pr.warnings.begin(), pr.warnings.end());
}

std::string CallGraph::languageOf(const std::string& qualified) const {
    const RepoFunction* fn = find(qualified);
    if (fn == nullptr) return std::string();
    const LanguageProfile* lp = profileForPath(fn->file);
    return lp ? lp->id : std::string();
}

std::string CallGraph::resolveCallee(const CallEdge& e, bool allowCrossLanguage,
                                    bool& rejectedCrossLanguage) const {
    rejectedCrossLanguage = false;

    // The separator comes from the caller's language, discovered from the file
    // the call site sits in.
    const LanguageProfile* lang = profileForPath(e.evidence.file);
    const std::string sep = lang ? lang->scopeSeparator : "::";
    const std::string callerLang = languageOf(e.caller);

    // A candidate is only acceptable when it belongs to the same language as
    // the call site. Without this, a name that happens to be unique across the
    // whole repository but lives in another language wins the fallback and
    // fabricates an edge.
    auto sameLanguage = [&](const std::string& cand) -> bool {
        if (allowCrossLanguage) return true;
        const std::string candLang = languageOf(cand);
        if (callerLang.empty() || candLang.empty()) return true;   // unknown: allow
        return candLang == callerLang;
    };

    // 1. Scope-chain candidates, filtered by language. The "@lang" suffix used
    //    to disambiguate cross-language name collisions is appended here so a
    //    suffixed node is still reachable.
    const std::string suffix = lang ? ("@" + lang->id) : std::string();
    bool sawForeignCandidate = false;
    auto consider = [&](const std::string& cand) -> const std::string* {
        auto it = funcs_.find(cand);
        if (it == funcs_.end()) return nullptr;
        if (sameLanguage(cand)) return &it->first;
        sawForeignCandidate = true;
        return nullptr;
    };
    std::vector<std::string> candidates = candidateNames(e.caller, e.calleeQualified, sep);

    // Prefer the suffixed form. A suffixed node exists only because the bare
    // name was already taken by another language, so the suffix is the more
    // specific match - without it, "main()" inside a Python file resolves to the
    // Java main that happens to own the bare key.
    //
    // (An earlier version of this loop contained a stray `if (suffix.empty())
    // break;` inside the body, which exited after the first candidate and made
    // the whole suffixed lookup dead code.)
    if (!suffix.empty()) {
        for (const std::string& cand : candidates) {
            const std::string suffixed = cand + suffix;
            auto it = funcs_.find(suffixed);
            if (it != funcs_.end() && sameLanguage(suffixed)) return suffixed;
        }
    }
    for (const std::string& cand : candidates) {
        if (const std::string* r = consider(cand)) return *r;
    }

    // 2. Unique short-name fallback, also filtered by language. This is what
    //    makes a Python obj.save() resolvable when exactly one save() exists in
    //    the Python sources.
    auto tryUnique = [&](const std::string& name) -> std::string {
        auto it = byShortName_.find(name);
        if (it == byShortName_.end()) return std::string();
        // Count matches in the caller's language only.
        std::string found;
        int inLanguage = 0;
        for (const std::string& cand : it->second) {
            if (sameLanguage(cand)) {
                found = cand;
                ++inLanguage;
            } else {
                sawForeignCandidate = true;
            }
        }
        if (inLanguage == 1) return found;
        // Exactly one definition exists in the whole repo, but it is in another
        // language: refuse it rather than invent a cross-language call.
        if (inLanguage == 0 && it->second.size() >= 1) return std::string();
        return std::string();
    };

    if (std::string r = tryUnique(e.callee); !r.empty()) return r;
    if (std::string r = tryUnique(tailIdent(e.calleeQualified)); !r.empty()) return r;

    // 3. Constructor call. In Python (and Java) a class name is not itself a
    //    node in the graph - only its methods are - so `UserService(...)` used
    //    to resolve to nothing and the whole caller chain above it was lost.
    //    Map a class name onto its initialiser.
    auto tryConstructor = [&](const std::string& className) -> std::string {
        if (className.empty()) return std::string();
        static const char* ctorNames[] = {"__init__", "<init>", "constructor"};
        for (const char* ctor : ctorNames) {
            const std::string cand = className + "." + ctor;
            if (funcs_.count(cand) && sameLanguage(cand)) return cand;
            // C++ style would be Class::Class
            const std::string cand2 = className + "::" + className;
            if (funcs_.count(cand2) && sameLanguage(cand2)) return cand2;
        }
        return std::string();
    };
    if (std::string r = tryConstructor(e.callee); !r.empty()) return r;
    if (std::string r = tryConstructor(tailIdent(e.calleeQualified)); !r.empty()) return r;

    if (sawForeignCandidate) rejectedCrossLanguage = true;
    return std::string();
}

void CallGraph::resolve(bool allowCrossLanguage) {
    edges_.clear();
    crossLanguageRejected_ = 0;
    std::set<std::pair<std::string, std::string>> seen;
    size_t unresolved = 0;

    for (CallEdge e : pending_) {
        bool rejectedCross = false;
        std::string target = resolveCallee(e, allowCrossLanguage, rejectedCross);
        if (target.empty()) {
            ++unresolved;
            if (rejectedCross) ++crossLanguageRejected_;
            continue;
        }
        auto key = std::make_pair(e.caller, target);
        if (!seen.insert(key).second) continue;   // dedupe identical edges
        e.calleeQualified = target;
        e.resolved = true;
        edges_.push_back(std::move(e));
    }

    unresolvedEdges_ = unresolved;

    // Tell the user that edges were withheld, rather than silently dropping
    // them: "unresolved" and "resolved to something in another language" are
    // different facts and only the first is a lower-bound caveat.
    if (crossLanguageRejected_ > 0) {
        warnings_.push_back(
            std::to_string(crossLanguageRejected_) +
            " call site(s) matched a definition in a DIFFERENT language and were "
            "rejected. Real cross-language calls go through FFI/JNI/sockets and do "
            "not appear as direct calls; accepting these would fabricate edges. "
            "Pass --allow-cross-language to include them.");
    }

    std::sort(edges_.begin(), edges_.end(), [](const CallEdge& a, const CallEdge& b) {
        if (a.caller != b.caller) return a.caller < b.caller;
        return a.calleeQualified < b.calleeQualified;
    });
}

const RepoFunction* CallGraph::find(const std::string& qualified) const {
    auto it = funcs_.find(qualified);
    return it == funcs_.end() ? nullptr : &it->second;
}

bool CallGraph::has(const std::string& qualified) const {
    return funcs_.count(qualified) > 0;
}

std::vector<std::string> CallGraph::callersOf(const std::string& qualified) const {
    std::vector<std::string> out;
    for (const auto& e : edges_) {
        if (e.calleeQualified == qualified) out.push_back(e.caller);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> CallGraph::calleesOf(const std::string& qualified) const {
    std::vector<std::string> out;
    for (const auto& e : edges_) {
        if (e.caller == qualified) out.push_back(e.calleeQualified);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<const CallEdge*> CallGraph::edgesFrom(const std::string& qualified) const {
    std::vector<const CallEdge*> out;
    for (const auto& e : edges_) {
        if (e.caller == qualified) out.push_back(&e);
    }
    return out;
}

std::vector<std::string> CallGraph::findByShortName(const std::string& shortName) const {
    auto it = byShortName_.find(shortName);
    return it == byShortName_.end() ? std::vector<std::string>{} : it->second;
}

std::vector<const RepoFunction*> CallGraph::allFunctions() const {
    std::vector<const RepoFunction*> out;
    out.reserve(funcs_.size());
    for (const auto& kv : funcs_) out.push_back(&kv.second);
    return out;
}

GraphStats CallGraph::stats() const {
    GraphStats s;
    s.functions = funcs_.size();
    s.edges = edges_.size();
    s.unresolvedEdges = unresolvedEdges_;
    s.perLanguage = perLanguage_;
    s.resolvedEdges = edges_.size();
    for (const auto& kv : funcs_) {
        if (kv.second.isTest) ++s.tests;
        if (kv.second.isEntryPoint) ++s.entryPoints;
        if (kv.second.isPublicApi) ++s.publicApi;
    }
    for (const auto& kv : byShortName_) {
        if (kv.second.size() > 1) ++s.ambiguousNames;
    }

    // Attribute each function to its language through the file it lives in, so
    // the per-language table reports real function counts instead of zero.
    for (const auto& kv : funcs_) {
        const LanguageProfile* lp = profileForPath(kv.second.file);
        if (lp == nullptr) continue;
        s.perLanguage[lp->displayName].functions++;
    }
    return s;
}

}  // namespace rippletrace
