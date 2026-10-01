// ============================================================
//  RippleTrace - tokenizer.h
//  Lightweight C++ lexer used by the concept-stage parser.
//
//  Design note: RippleTrace's analysis core only needs (a) function
//  signatures/bodies and (b) call sites. A full AST is not required
//  for the concept version, so this lexer deliberately stays small and
//  dependency-free. The IRepoParser interface in parser.h is the seam
//  where a libclang-based frontend can be dropped in later without
//  touching the graph/traversal/report code.
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace rippletrace {

enum class TokKind {
    Identifier,
    Number,
    StringLit,
    CharLit,
    Punct,
    EndOfFile
};

struct Token {
    TokKind kind = TokKind::EndOfFile;
    std::string text;   // raw lexeme ("+=", "value", "\"x\"")
    int line = 0;       // 1-based
    int col = 0;        // 1-based
    bool precededByNewline = false;
};

// Comment and literal syntax is a language property, so it is passed in rather
// than assumed. Getting this wrong is not cosmetic: with the C++ rules, a
// Python file's "#" comment lexes as code and its contents become call sites.
struct TokenizeOptions {
    std::string lineComment = "//";     // "//" for C-family, "#" for Python
    bool hasBlockComment = true;        // false for Python
    bool hasPreprocessor = true;        // false for Java and Python
    bool rawStrings = true;             // C++ raw string literals R"(...)"
};

// Tokenize a whole translation unit. Comments are dropped, string and
// character literals are preserved as single tokens so that braces or
// parentheses inside them never disturb structural scanning.
std::vector<Token> tokenize(const std::string& source,
                            const TokenizeOptions& opt = TokenizeOptions());

// True when the identifier is a reserved word that must never be treated
// as a function name or a call target.
bool isKeyword(const std::string& id);

}  // namespace rippletrace
