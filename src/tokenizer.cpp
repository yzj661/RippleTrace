// ============================================================
//  RippleTrace - tokenizer.cpp
// ============================================================
#include "tokenizer.h"

#include <cctype>
#include <unordered_set>

namespace rippletrace {

namespace {

const std::unordered_set<std::string>& keywordTable() {
    static const std::unordered_set<std::string> keywords = {
        // control flow
        "if", "else", "for", "while", "do", "switch", "case", "default",
        "break", "continue", "return", "goto", "try", "catch", "throw",
        // declarations / types we never want as call targets
        "sizeof", "alignof", "decltype", "new", "delete", "this", "operator",
        "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
        "constexpr", "consteval", "noexcept", "static_assert", "typedef",
        // qualifiers and specifiers (harmless to skip as call names)
        "const", "volatile", "static", "inline", "virtual", "explicit",
        "friend", "public", "private", "protected", "class", "struct",
        "union", "enum", "namespace", "using", "template", "typename",
        "auto", "void", "bool", "char", "short", "int", "long", "float",
        "double", "unsigned", "signed", "wchar_t", "char8_t", "char16_t",
        "char32_t", "true", "false", "nullptr", "override", "final",
        "mutable", "register", "extern", "mutable", "thread_local",
    };
    return keywords;
}

inline bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;  // allow non-ASCII identifiers
}

inline bool isIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

// Length in bytes of the UTF-8 sequence starting at `c`, or 0 when `c` is not
// a valid lead byte.
//
// This matters far beyond tidiness. Treating every byte >= 0x80 as an
// identifier character splits a multi-byte character into several one-byte
// tokens, and reassembling those tokens during signature extraction can cut the
// character in half. The result is an invalid byte sequence, which then breaks
// the JSON encoding on the way to a model endpoint:
//
//     HTTP 400 ... lone leading surrogate in hex escape
//
// Consuming whole sequences keeps every emitted string well-formed.
inline int utf8SequenceLength(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 0;   // stray continuation byte or invalid lead: handle as a unit
}

}  // namespace

bool isKeyword(const std::string& id) {
    return keywordTable().count(id) > 0;
}

std::vector<Token> tokenize(const std::string& src) {
    return tokenize(src, TokenizeOptions());
}

std::vector<Token> tokenize(const std::string& src, const TokenizeOptions& opt) {
    std::vector<Token> out;
    out.reserve(src.size() / 4 + 16);

    const size_t n = src.size();
    size_t i = 0;
    int line = 1;
    int col = 1;
    bool sawNewline = false;

    auto advance = [&](size_t k) {
        for (size_t t = 0; t < k && i < n; ++t) {
            if (src[i] == '\n') { ++line; col = 1; sawNewline = true; }
            else { ++col; }
            ++i;
        }
    };

    auto push = [&](TokKind kind, const std::string& text, int l, int c) {
        Token tk;
        tk.kind = kind;
        tk.text = text;
        tk.line = l;
        tk.col = c;
        tk.precededByNewline = sawNewline;
        out.push_back(std::move(tk));
        sawNewline = false;
    };

    while (i < n) {
        char c = src[i];

        // whitespace
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(1); continue; }

        // preprocessor directives: skip the whole logical line so that
        // #include / #define contents never become identifiers we analyse.
        // Only C-family languages have a preprocessor; in Python '#' starts a
        // line comment, which the branch below handles.
        if (c == '#' && opt.hasPreprocessor) {
            while (i < n && src[i] != '\n') {
                // honour line continuations inside macros
                if (src[i] == '\\' && i + 1 < n && src[i + 1] == '\n') { advance(2); continue; }
                if (src[i] == '\\' && i + 2 < n && src[i + 1] == '\r' && src[i + 2] == '\n') { advance(3); continue; }
                advance(1);
            }
            continue;
        }

        // comments - syntax depends on the language
        if (!opt.lineComment.empty() && src.compare(i, opt.lineComment.size(),
                                                    opt.lineComment) == 0) {
            while (i < n && src[i] != '\n') advance(1);
            continue;
        }
        if (opt.hasBlockComment && c == '/' && i + 1 < n && src[i + 1] == '*') {
            advance(2);
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) advance(1);
            if (i + 1 < n) advance(2);
            continue;
        }

        const int tl = line, tc = col;

        // identifiers / keywords. Non-ASCII runs are consumed one whole UTF-8
        // sequence at a time so a token never ends mid-character.
        if (isIdentStart(c)) {
            size_t s = i;
            while (i < n) {
                unsigned char u = static_cast<unsigned char>(src[i]);
                if (u < 0x80) {
                    if (!isIdentPart(src[i])) break;
                    advance(1);
                    continue;
                }
                int len = utf8SequenceLength(u);
                if (len <= 0) { advance(1); continue; }   // stray byte
                // Only consume the sequence if all continuation bytes are
                // present; otherwise take one byte so we make progress.
                bool complete = (i + static_cast<size_t>(len) <= n);
                if (complete) {
                    for (int k = 1; k < len; ++k) {
                        unsigned char cc = static_cast<unsigned char>(src[i + k]);
                        if (cc < 0x80 || cc > 0xBF) { complete = false; break; }
                    }
                }
                if (!complete) { advance(1); continue; }
                advance(static_cast<size_t>(len));
            }
            push(TokKind::Identifier, src.substr(s, i - s), tl, tc);
            continue;
        }

        // numbers (kept opaque; we never evaluate them)
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            size_t s = i;
            while (i < n && (isIdentPart(src[i]) || src[i] == '.')) advance(1);
            push(TokKind::Number, src.substr(s, i - s), tl, tc);
            continue;
        }

        // string literal (raw or normal), keep as one token
        if (c == '"') {
            size_t s = i;
            advance(1);
            while (i < n) {
                if (src[i] == '\\') { advance(2); continue; }
                if (src[i] == '"') { advance(1); break; }
                advance(1);
            }
            push(TokKind::StringLit, src.substr(s, i - s), tl, tc);
            continue;
        }
        if (c == 'R' && i + 1 < n && src[i + 1] == '"') {
            size_t s = i;
            advance(2);
            // find the delimiter up to '('
            std::string delim;
            while (i < n && src[i] != '(') { delim.push_back(src[i]); advance(1); }
            if (i < n) advance(1);
            std::string terminator = ")" + delim + "\"";
            while (i < n && src.compare(i, terminator.size(), terminator) != 0) advance(1);
            if (i < n) advance(terminator.size());
            push(TokKind::StringLit, src.substr(s, i - s), tl, tc);
            continue;
        }

        // character literal
        if (c == '\'') {
            size_t s = i;
            advance(1);
            while (i < n) {
                if (src[i] == '\\') { advance(2); continue; }
                if (src[i] == '\'') { advance(1); break; }
                advance(1);
            }
            push(TokKind::CharLit, src.substr(s, i - s), tl, tc);
            continue;
        }

        // multi-character punctuation, longest match first
        static const char* multi[] = {
            "->*", "<<=", ">>=", "...", "::", "->", "++", "--", "<<", ">>",
            "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=",
            "%=", "&=", "|=", "^=", ".*"
        };
        bool matched = false;
        for (const char* m : multi) {
            size_t len = std::char_traits<char>::length(m);
            if (src.compare(i, len, m) == 0) {
                advance(len);
                push(TokKind::Punct, std::string(m), tl, tc);
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // single character punctuation
        advance(1);
        push(TokKind::Punct, std::string(1, c), tl, tc);
    }

    Token eof;
    eof.kind = TokKind::EndOfFile;
    eof.line = line;
    eof.col = col;
    out.push_back(eof);
    return out;
}

}  // namespace rippletrace
