// ============================================================
//  RippleTrace - parser.cpp
//  Token + brace driven function and call-site extraction.
// ============================================================
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace rippletrace {

namespace {

// Keywords that appear before '(' but are not calls, taken from the language
// profile so the same code serves C++ and Java.
bool isNonCallKeyword(const LanguageProfile& lang, const std::string& name) {
    return lang.nonCallKeywords.count(name) > 0;
}

bool isBlockIntroducer(const LanguageProfile& lang, const std::string& name) {
    return lang.blockIntroducers.count(name) > 0;
}

bool isPunct(const Token& t, const char* p) {
    return t.kind == TokKind::Punct && t.text == p;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool containsCI(const std::string& hay, const char* needle) {
    return toLower(hay).find(toLower(needle)) != std::string::npos;
}

bool looksLikeTestFile(const std::string& path) {
    std::string p = toLower(path);
    // normalise separators
    std::replace(p.begin(), p.end(), '\\', '/');
    const char* markers[] = {"/test/", "/tests/", "test_", "_test.", "test.", ".test",
                             "spec.", "_spec.", "/spec/"};
    for (const char* m : markers) {
        if (p.find(m) != std::string::npos) return true;
    }
    return false;
}

bool looksLikeTestFunction(const std::string& name) {
    if (name.rfind("test", 0) == 0 || name.rfind("Test", 0) == 0) return true;
    if (name.rfind("TEST", 0) == 0) return true;
    if (name.size() > 4) {
        std::string tail = name.substr(name.size() - 4);
        if (tail == "Test" || tail == "test") return true;
    }
    if (name.find("_test") != std::string::npos) return true;
    return false;
}

bool isPublicHeader(const std::string& path, bool isHeader) {
    if (!isHeader) return false;
    std::string p = toLower(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    // Normalise so patterns match both "internal/x.h" and "/internal/x.h".
    std::string q = "/" + p;

    // Test fixtures and internal headers are not part of the public surface.
    if (q.find("/test") != std::string::npos) return false;
    if (q.find("/internal/") != std::string::npos) return false;
    if (q.find("/detail/") != std::string::npos) return false;
    if (q.find("/src/") != std::string::npos) return false;
    if (p.rfind("src/", 0) == 0) return false;
    if (p.find("_internal.h") != std::string::npos) return false;
    // a header at repo root or under include/ is treated as public surface
    return true;
}

// Walk backwards from the '(' token to recover `name` and any `Scope::`
// qualifier. Returns false when the construct is not a callable.
bool recoverCallee(const LanguageProfile& lang, const std::vector<Token>& toks, size_t openParenIdx,
                   std::string& outQualified, std::string& outKind) {
    if (openParenIdx == 0) return false;
    size_t i = openParenIdx;
    if (toks[i - 1].kind != TokKind::Identifier) return false;
    std::string name = toks[i - 1].text;
    if (isKeyword(name)) return false;
    if (isNonCallKeyword(lang, name)) return false;

    // skip template arguments directly attached to the name: foo<T>(...)
    i -= 1;

    std::string scope;
    while (i >= 2 && isPunct(toks[i - 1], "::") && toks[i - 2].kind == TokKind::Identifier) {
        if (!scope.empty()) scope = "::" + scope;
        scope = toks[i - 2].text + scope;
        i -= 2;
    }

    // Distinguish "obj.method(" / "ptr->method(" from a free function.
    std::string kind = "direct";
    if (i >= 1) {
        const Token& prev = toks[i - 1];
        if (isPunct(prev, ".") || isPunct(prev, "->")) kind = "member";
    }
    if (scope.empty()) {
        // A preceding "::" with no identifier means global scope (::foo)
        if (i >= 1 && isPunct(toks[i - 1], "::")) kind = "direct";
    } else {
        kind = "qualified";
    }

    outQualified = scope.empty() ? name : (scope + "::" + name);
    outKind = kind;
    return true;
}

// ---------------------------------------------------------------------------
//  Constructor initializer list detection.
//
//  In
//      Foo::Foo(int x) : a_(x), b_(y) {}
//      Foo::Foo(std::string h) : host_(std::move(h)) {}
//  my brace-walk sees "a_(x)" followed later by '{' and would otherwise record
//  a phantom function named "a_". The distinguishing feature is the token
//  immediately before the identifier: in an initializer list it is ':' or ','
//  at declaration level, which can never precede a function name. A '::' before
//  the identifier means it is a qualified name, not an initializer.
// ---------------------------------------------------------------------------
bool colonSeparated(const std::vector<Token>& toks, size_t openParen) {
    if (openParen < 2) return false;
    size_t k = openParen - 1;
    if (toks[k].kind != TokKind::Identifier) return false;
    --k;   // token before the identifier
    if (isPunct(toks[k], "::")) return false;   // Qualified::name(
    return isPunct(toks[k], ":") || isPunct(toks[k], ",");
}

// ---------------------------------------------------------------------------
//  Locate the parameter list belonging to the brace at `braceIdx`.
//
//  The '(' and ')' are NOT necessarily adjacent to the '{'. C++ allows
//  trailing qualifiers, and the common ones are very common:
//
//      void f() { }                 ')' and '{' adjacent
//      void f() const { }           'const' sits between them
//      void f() noexcept { }
//      void f() override { }
//      void f() const noexcept override { }
//      auto f() -> T { }            trailing return type
//
//  An earlier version required the token before '{' to be ')'. That silently
//  dropped EVERY const member function - two of them in impact.cpp, where the
//  loss also swallowed everything after them. Scanning back over the qualifier
//  run is the fix.
//
//  Returns the index of the matching '(' , or npos when the brace is not a body.
// ---------------------------------------------------------------------------
size_t findParamList(const std::vector<Token>& toks, size_t braceIdx) {
    size_t p = braceIdx;
    int guard = 0;
    while (p > 0 && guard++ < 32) {
        --p;
        const Token& q = toks[p];
        if (isPunct(q, ")")) break;
        if (q.kind == TokKind::Punct) {
            // '->' introduces a trailing return type; other punctuation means
            // this brace is not a function body.
            if (isPunct(q, "->")) continue;
            return std::string::npos;
        }
        if (q.kind != TokKind::Identifier) return std::string::npos;
        // Identifiers here are qualifiers or a trailing return type name.
    }
    if (p == 0 || !isPunct(toks[p], ")")) return std::string::npos;

    int pd = 0;
    for (size_t k = p + 1; k-- > 0;) {
        if (isPunct(toks[k], ")")) ++pd;
        else if (isPunct(toks[k], "(")) {
            --pd;
            if (pd == 0) return k;
        }
    }
    return std::string::npos;
}

}  // namespace

std::string collapseWhitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (char c : s) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prevSpace && !out.empty()) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void BraceParser::parseUnit(const std::string& repoRelativePath,
                                const std::string& source,
                                bool isHeader,
                                ParseResult& out) {
    TokenizeOptions tokOpt;
    tokOpt.lineComment = lang_->lineComment;
    tokOpt.hasBlockComment = lang_->hasBlockComment;
    tokOpt.hasPreprocessor = (lang_->id == "cpp");
    tokOpt.rawStrings = (lang_->id == "cpp");
    std::vector<Token> toks = tokenize(source, tokOpt);
    const size_t n = toks.size();
    if (n == 0) return;

    const bool testFile = looksLikeTestFile(repoRelativePath);
    const bool publicHeader = isPublicHeader(repoRelativePath, isHeader);

    // Depth of '{' we are currently inside at top level of this unit.
    int depth = 0;
    // Stack of function scans currently open; size == number of nested
    // function bodies we are inside. A body discovered while this is
    // non-empty is a lambda/local class and is intentionally ignored.
    struct OpenScan {
        RepoFunction fn;
        std::vector<CallEdge> calls;
        int branchPoints = 0;
        int braceDepthEntered = 0;
    };
    std::vector<OpenScan> stack;

    // Class / interface scopes currently open, as (name, brace depth at which
    // the class body started). Needed because a Java method carries no
    // qualification in its own source - `boolean addUser(...)` inside
    // `class Service` - so without this every Java method became a bare name
    // ("addUser") instead of "Service.addUser", which made the class-qualified
    // form in the CLI unreachable and let same-named methods in different
    // classes collide. C++ writes Foo::bar explicitly, so this only ever adds
    // information there.
    std::vector<std::pair<std::string, int>> classScopes;
    // Set when the '{' just processed opened a class/interface/enum body that
    // should be tracked (as opposed to a control-flow block).
    int pendingClassAtDepth = -1;
    std::string pendingClassName;

    for (size_t idx = 0; idx < n; ++idx) {
        const Token& t = toks[idx];

        if (isPunct(t, "{")) {
            ++depth;

            // Is this brace opening a function body? The parameter list may not
            // be adjacent (trailing qualifiers like 'const' / 'noexcept').
            size_t openParen = findParamList(toks, idx);
            bool isBody = (openParen != std::string::npos);

            // Was this brace preceded by a class/interface/enum name? If so the
            // body about to open is a type scope, not a function.
            if (openParen == std::string::npos && idx >= 1 &&
                toks[idx - 1].kind == TokKind::Identifier) {
                // The token before the name must be the introducer keyword.
                const std::string& nameTok = toks[idx - 1].text;
                if (idx >= 2 && toks[idx - 2].kind == TokKind::Identifier) {
                    const std::string& kw = toks[idx - 2].text;
                    if (kw == "class" || kw == "interface" || kw == "enum" ||
                        kw == "record" || kw == "struct" || kw == "union") {
                        pendingClassAtDepth = depth;
                        pendingClassName = nameTok;
                    }
                }
            }

            if (isBody && stack.empty()) {
                // verify the construct is not a control statement / block intro
                std::string callee, kind;
                bool callable = recoverCallee(*lang_, toks, openParen, callee, kind);
                bool blocked = false;
                if (openParen >= 1 && toks[openParen - 1].kind == TokKind::Identifier) {
                    const std::string& beforeParen = toks[openParen - 1].text;
                    if (isBlockIntroducer(*lang_, beforeParen)) blocked = true;
                    if (beforeParen == "decltype") blocked = true;
                }

                // A function declarator is "(params)". A function-like *macro*
                // invocation is "(literal, ...)" -- e.g. TB_CASE("name"). The
                // token before the '(' is then a string literal, which can never
                // appear in a declarator. Reject those so a trailing block after
                // a macro call is not mistaken for a function body.
                for (size_t k = openParen; k-- > 0;) {
                    const Token& q = toks[k];
                    if (q.kind == TokKind::StringLit || q.kind == TokKind::CharLit ||
                        q.kind == TokKind::Number) {
                        blocked = true;
                    }
                    break;   // inspect only the token immediately before '('
                }

                // Reject a brace that is not the declaration's body brace:
                //   Foo::Foo(int x) : a_(x) { ... }   -> "a_" is an initializer,
                //                                        not a function
                //   const int K[] = {1, 2, 3};        -> aggregate initialiser
                {
                    size_t closeParen = idx - 1;   // the ')' immediately before '{'
                    for (size_t k = closeParen + 1; k < idx; ++k) {
                        const Token& q = toks[k];
                        if (isPunct(q, "=") || isPunct(q, "{")) blocked = true;
                    }
                    if (colonSeparated(toks, openParen)) blocked = true;
                }

                // Type/namespace definitions are not functions. Detect them by the
                // introducer keyword sitting before the last identifier of the
                // construct:  class Foo {  /  struct Bar {  /  namespace ns {
                //
                // This scan MUST stop at a statement or block boundary. An earlier
                // version walked backwards over ';' and '}' and skipped over any
                // non-identifier token, so inside
                //
                //     void f(...) const {        <- the '{' being classified
                //         switch (r) { ... }
                //     }                          <- '}' crossed
                //     ... resolve(const std::string& name) const {
                //
                // it reached a *previous* function's name, found "resolve" before
                // the '(' and treated the whole declaration as a type, dropping the
                // function and everything after it. Two functions in impact.cpp
                // were lost that way.
                {
                    size_t nm = openParen;
                    while (nm > 0 && toks[nm - 1].kind != TokKind::Identifier) --nm;
                    if (nm > 0) {
                        size_t j = nm - 1;   // the identifier itself
                        while (j > 0) {
                            --j;
                            const Token& q = toks[j];
                            if (q.kind == TokKind::Identifier) {
                                if (q.text == "class" || q.text == "struct" ||
                                    q.text == "union" || q.text == "enum" ||
                                    q.text == "namespace") {
                                    blocked = true;
                                }
                                break;   // first identifier decides it
                            }
                            // A boundary means we are looking at a new declaration.
                            if (isPunct(q, ";") || isPunct(q, "}") || isPunct(q, "{")) break;
                            if (q.kind == TokKind::EndOfFile) break;
                        }
                    }
                }

                if (callable && !blocked) {
                    OpenScan scan;
                    RepoFunction& f = scan.fn;

                    // Qualify with the enclosing class when the language does not
                    // write it out. Java declares `boolean addUser(...)` inside
                    // `class Service`, so without this the id would be a bare
                    // "addUser"; C++ already writes Foo::bar and is left alone.
                    std::string qualifiedName = callee;
                    if (qualifiedName.find("::") == std::string::npos &&
                        qualifiedName.find('.') == std::string::npos &&
                        !classScopes.empty() &&
                        classScopes.back().second < depth) {
                        const std::string& cls = classScopes.back().first;
                        // A constructor is already written as the class name, so
                        // prefixing it produced "Service.Service".
                        if (qualifiedName == cls) {
                            qualifiedName = cls + "." + cls;
                        } else {
                            qualifiedName = cls + "." + qualifiedName;
                        }
                    }
                    f.qualified = qualifiedName;
                    size_t colon = qualifiedName.rfind('.');
                    size_t colon2 = qualifiedName.rfind("::");
                    size_t sep = std::max(colon == std::string::npos ? 0 : colon + 1,
                                          colon2 == std::string::npos ? 0 : colon2 + 2);
                    f.name = (sep == 0) ? qualifiedName : qualifiedName.substr(sep);
                    f.file = repoRelativePath;
                    f.declLine = toks[openParen].line;
                    f.bodyLineStart = t.line;
                    f.isDefinition = true;
                    f.isTest = testFile || looksLikeTestFunction(f.name);
                    f.isEntryPoint = (f.name == "main") || f.isTest;
                    f.isPublicApi = publicHeader;

                    // Signature: walk back from the parameter list to the start of
                    // the declaration. This intentionally crosses line breaks so
                    // multi-line signatures are captured, and stops at the previous
                    // ';', '}', or '{' which bound the declaration.
                    size_t sigStart = openParen;
                    while (sigStart > 0) {
                        const Token& q = toks[sigStart - 1];
                        if (isPunct(q, ";") || isPunct(q, "}") || isPunct(q, "{")) break;
                        if (q.kind == TokKind::Punct && q.text == ":") {
                            // could be "public:" etc.; keep going, the ';'/'}'/'{' rule
                            // handles the real boundaries
                        }
                        --sigStart;
                    }
                    std::string sig;
                    for (size_t k = sigStart; k <= openParen; ++k) {
                        if (!sig.empty()) sig.push_back(' ');
                        sig += toks[k].text;
                    }
                    f.signature = collapseWhitespace(sig);
                    if (!f.signature.empty() && f.signature.size() > 200) {
                        // Truncate on a UTF-8 boundary: cutting at an arbitrary
                        // byte leaves a partial character, which later breaks
                        // JSON encoding on its way to a model endpoint.
                        size_t cut = 197;
                        while (cut > 0 &&
                               (static_cast<unsigned char>(f.signature[cut]) & 0xC0) == 0x80) {
                            --cut;
                        }
                        f.signature = f.signature.substr(0, cut) + "...";
                    }

                    scan.braceDepthEntered = depth;
                    stack.push_back(std::move(scan));
                    continue;
                }
            }

            // Commit a pending class scope once its body brace is in place. Done
            // here so a method's own '{' (handled above) is not mistaken for the
            // class body.
            if (pendingClassAtDepth == depth && !pendingClassName.empty()) {
                classScopes.emplace_back(pendingClassName, depth);
                pendingClassAtDepth = -1;
                pendingClassName.clear();
            }
            continue;
        }

        if (isPunct(t, "}")) {
            if (!stack.empty() && depth == stack.back().braceDepthEntered) {
                OpenScan scan = std::move(stack.back());
                stack.pop_back();

                RepoFunction f = scan.fn;
                f.bodyLineEnd = t.line;
                f.lineCount = f.bodyLineEnd - f.bodyLineStart + 1;
                f.cyclomatic = 1 + scan.branchPoints;

                // unique callees
                std::unordered_set<std::string> uniq;
                for (const auto& e : scan.calls) uniq.insert(e.calleeQualified);
                f.directCallCount = static_cast<int>(uniq.size());

                out.functions.push_back(f);
                for (auto& e : scan.calls) out.edges.push_back(std::move(e));
            }
            // Leaving a class body closes its scope.
            while (!classScopes.empty() && classScopes.back().second >= depth) {
                classScopes.pop_back();
            }
            --depth;
            if (depth < 0) depth = 0;
            continue;
        }

        if (stack.empty()) continue;

        OpenScan& cur = stack.back();

        // -------- metrics --------
        if (t.kind == TokKind::Identifier &&
            (t.text == "if" || t.text == "for" || t.text == "while" ||
             t.text == "case" || t.text == "catch" || t.text == "&&" ||
             t.text == "||" || t.text == "?")) {
            ++cur.branchPoints;
        }

        // -------- call sites --------
        if (isPunct(t, "(") && idx > 0) {
            // ignore the parameter list of the definition itself
            if (!cur.calls.empty() || idx != 0) {
                const Token& prev = toks[idx - 1];
                if (prev.kind == TokKind::Identifier) {
                    std::string callee, kind;
                    if (recoverCallee(*lang_, toks, idx, callee, kind)) {
                        // Skip the record of the function's own name followed by
                        // its parameter list: that '(' is the definition's, which
                        // sits at or before the body brace.
                        bool isOwnSignature =
                            (callee == cur.fn.qualified &&
                             toks[idx].line == cur.fn.declLine);
                        if (!isOwnSignature) {
                            CallEdge e;
                            e.caller = cur.fn.qualified;
                            e.callee = prev.text;
                            e.calleeQualified = callee;
                            e.evidence.file = repoRelativePath;
                            e.evidence.line = t.line;
                            e.evidence.kind = kind;
                            cur.calls.push_back(std::move(e));
                        }
                    }
                }
            }
        }
    }

    // Unterminated definitions (parse anomalies) are reported, not hidden.
    for (const auto& scan : stack) {
        out.warnings.push_back("unterminated function body: " + scan.fn.qualified +
                               " in " + repoRelativePath);
    }
}

// ============================================================
//  Python: indentation-scoped extraction.
//
//  Python has no braces, so the block structure comes from indentation:
//
//      @decorator
//      def name(params) -> type:
//          body
//
//  Classes add a scope level, so  class Foo: / def bar(self):  becomes Foo.bar.
//  Nested defs are closures and keep the enclosing name in their id, because a
//  closure's calls still affect the function that contains it.
//
//  Deliberate limitation: a call like  obj.save()  carries no static type in
//  Python, so it can only be resolved when the method name is unique in the
//  project. That fallback lives in the graph layer, not here.
// ============================================================
void IndentParser::parseUnit(const std::string& repoRelativePath,
                             const std::string& source,
                             bool isHeader,
                             ParseResult& out) {
    (void)isHeader;   // Python has no header/body split

    TokenizeOptions tokOpt;
    tokOpt.lineComment = lang_->lineComment;
    tokOpt.hasBlockComment = lang_->hasBlockComment;
    tokOpt.hasPreprocessor = false;
    tokOpt.rawStrings = false;

    const std::vector<Token> toks = tokenize(source, tokOpt);
    const size_t n = toks.size();
    if (n == 0) return;

    const bool testFile = looksLikeTestFile(repoRelativePath);

    // Leading-whitespace width per line, recovered from the raw source because
    // the token stream has whitespace removed.
    std::vector<int> indentOfLine(2, 0);
    {
        int curLine = 1;
        int curIndent = 0;
        bool atLineStart = true;
        for (char ch : source) {
            if (ch == '\n') {
                ++curLine;
                curIndent = 0;
                atLineStart = true;
                if (indentOfLine.size() <= static_cast<size_t>(curLine)) {
                    indentOfLine.resize(curLine + 1, 0);
                }
                indentOfLine[curLine] = 0;
            } else if (atLineStart && (ch == ' ' || ch == '\t')) {
                ++curIndent;
                if (indentOfLine.size() <= static_cast<size_t>(curLine)) {
                    indentOfLine.resize(curLine + 1, 0);
                }
                indentOfLine[curLine] = curIndent;
            } else if (atLineStart) {
                atLineStart = false;
            }
        }
    }
    auto indentAt = [&](int line) -> int {
        if (line < 0 || static_cast<size_t>(line) >= indentOfLine.size()) return 0;
        return indentOfLine[line];
    };

    struct OpenDef {
        RepoFunction fn;
        std::vector<CallEdge> calls;
        int branchPoints = 0;
        int indent = 0;   // indentation of the 'def' line
    };
    std::vector<OpenDef> stack;
    std::vector<std::pair<std::string, int>> classStack;   // (name, indent)

    // A definition is closed when a line appears whose indentation is at or
    // below the indentation of the 'def' line. The comparison used to be
    // `>= ind` with a caller passing lineIndent + 1, which never fired for
    // top-level definitions; see the call site for the full explanation.
    auto closeDefsShallowerThan = [&](int ind) {
        while (!stack.empty() && stack.back().indent >= ind) {
            OpenDef def = std::move(stack.back());
            stack.pop_back();

            RepoFunction f = def.fn;
            f.lineCount = std::max(1, f.bodyLineEnd - f.bodyLineStart + 1);
            f.cyclomatic = 1 + def.branchPoints;
            std::set<std::string> uniq;
            for (const auto& e : def.calls) uniq.insert(e.calleeQualified);
            f.directCallCount = static_cast<int>(uniq.size());

            out.functions.push_back(f);
            for (auto& e : def.calls) out.edges.push_back(std::move(e));
        }
    };

    for (size_t idx = 0; idx < n; ++idx) {
        const Token& t = toks[idx];
        if (t.kind == TokKind::EndOfFile) break;

        const int lineIndent = indentAt(t.line);

        // A definition is closed by a line whose indentation is at or below the
        // indentation of its 'def' line.
        //
        // This used to read `closeDefsShallowerThan(lineIndent + 1)`, which never
        // fired for a top-level def: its def line sits at indentation 0 and its
        // body at >= 1, so with the +1 nothing was ever <= the threshold and
        // every top-level def stayed open to end of file. The damage was severe:
        // the class and def stacks accumulated (producing ids like
        // "UserService.UserService.UserService...render") and bodyLineEnd was
        // overwritten on every subsequent line, so a 2-line function reported 72.
        if (t.precededByNewline) {
            closeDefsShallowerThan(lineIndent);
            while (!classStack.empty() && lineIndent <= classStack.back().second) {
                classStack.pop_back();
            }
        }
        // Only attribute the line to a def that survived the dedent above.
        if (!stack.empty()) stack.back().fn.bodyLineEnd = t.line;

        // ---- class definition ----
        if (t.kind == TokKind::Identifier && t.text == "class" &&
            idx + 1 < n && toks[idx + 1].kind == TokKind::Identifier) {
            classStack.emplace_back(toks[idx + 1].text, lineIndent);
            continue;
        }

        // ---- def / async def ----
        bool isDef = false;
        size_t defIdx = idx;
        if (t.kind == TokKind::Identifier && t.text == "async" &&
            idx + 1 < n && toks[idx + 1].text == "def") {
            isDef = true;
            defIdx = idx + 1;
        } else if (t.kind == TokKind::Identifier && t.text == "def") {
            isDef = true;
        }

        if (isDef && defIdx + 1 < n && toks[defIdx + 1].kind == TokKind::Identifier) {
            const std::string shortName = toks[defIdx + 1].text;

            // Qualified id = the enclosing class path plus the function name.
            //
            // For a nested function the enclosing def is NOT prepended: the
            // enclosing def's own id already starts with the same class path, so
            // joining the two produced "UserService.UserService.summary.render".
            // The class path plus the short name is unambiguous and stays stable
            // regardless of nesting depth.
            std::string qualified;
            for (const auto& c : classStack) {
                if (!qualified.empty()) qualified += ".";
                qualified += c.first;
            }
            if (!qualified.empty()) qualified += ".";
            qualified += shortName;

            OpenDef def;
            def.fn.qualified = qualified;
            def.fn.name = shortName;
            def.fn.file = repoRelativePath;
            def.fn.declLine = toks[defIdx + 1].line;
            def.fn.bodyLineStart = toks[defIdx + 1].line;
            def.fn.bodyLineEnd = toks[defIdx + 1].line;
            def.fn.isDefinition = true;
            def.fn.isTest = testFile || looksLikeTestFunction(shortName);
            def.fn.isEntryPoint = (shortName == "main") || def.fn.isTest;
            def.fn.isPublicApi = false;
            def.indent = lineIndent;

            std::string sig;
            for (size_t k = defIdx; k < n && toks[k].line == toks[defIdx].line; ++k) {
                if (toks[k].kind == TokKind::Punct && toks[k].text == ":") break;
                if (!sig.empty()) sig.push_back(' ');
                sig += toks[k].text;
            }
            def.fn.signature = collapseWhitespace(sig);

            stack.push_back(std::move(def));
            continue;
        }

        if (stack.empty()) continue;
        OpenDef& cur = stack.back();

        // ---- branch points ----
        if (t.kind == TokKind::Identifier &&
            (t.text == "if" || t.text == "elif" || t.text == "for" ||
             t.text == "while" || t.text == "except" || t.text == "and" ||
             t.text == "or")) {
            ++cur.branchPoints;
        }

        // ---- call sites ----
        if (t.kind == TokKind::Punct && t.text == "(" && idx > 0) {
            const Token& prev = toks[idx - 1];
            if (prev.kind == TokKind::Identifier && !isKeyword(prev.text) &&
                !isNonCallKeyword(*lang_, prev.text)) {
                if (prev.line == cur.fn.declLine) continue;   // the def's own header

                std::string callee = prev.text;
                std::string kind = "direct";

                if (idx >= 3 && toks[idx - 2].kind == TokKind::Punct &&
                    toks[idx - 2].text == "." &&
                    toks[idx - 3].kind == TokKind::Identifier) {
                    const std::string& recv = toks[idx - 3].text;
                    if (recv == "self" || recv == "cls") {
                        // self.method() means a method of the ENCLOSING CLASS,
                        // so the receiver resolves to the class path only.
                        //
                        // Using the current function's full id here produced
                        // "UserService.__init__.load" instead of
                        // "UserService.load" - a name that matches no definition,
                        // so the edge was dropped and impact failed to propagate
                        // from a method into the helper it calls.
                        std::string classPath;
                        for (const auto& c : classStack) {
                            if (!classPath.empty()) classPath += ".";
                            classPath += c.first;
                        }
                        callee = classPath.empty() ? prev.text
                                                   : (classPath + "." + prev.text);
                    } else {
                        callee = recv + "." + prev.text;
                    }
                    kind = "member";
                }

                CallEdge e;
                e.caller = cur.fn.qualified;
                e.callee = prev.text;
                e.calleeQualified = callee;
                e.evidence.file = repoRelativePath;
                e.evidence.line = t.line;
                e.evidence.kind = kind;
                cur.calls.push_back(std::move(e));
            }
        }
    }

    closeDefsShallowerThan(-1);
}

std::unique_ptr<IRepoParser> makeParser(const LanguageProfile& lang) {
    switch (lang.scopeStyle) {
        case ScopeStyle::Brace:
            return std::unique_ptr<IRepoParser>(new BraceParser(lang));
        case ScopeStyle::Indent:
            return std::unique_ptr<IRepoParser>(new IndentParser(lang));
    }
    return nullptr;
}

}  // namespace rippletrace
