#pragma once

// ============================================================================
//  UTF-8 well-formedness.
//
//  One implementation, shared by the source reader (repo.cpp) and the JSON
//  encoder (json.cpp). It has to be one implementation: there used to be two,
//  each checking only "are the continuation bytes in range?", and both
//  therefore accepted encoded surrogates.
//
//  A Visual Studio project on a Chinese Windows install supplied exactly such
//  a byte pair. CP936 decoded it to an unpaired UTF-16 surrogate, the Windows
//  API wrote that back out as WTF-8 (ED A0 80), it passed both validators,
//  reached the model prompt, and the endpoint rejected the request body:
//      HTTP 400 ... lone leading surrogate in hex escape
//
//  The lesson is that "well formed" and "valid" are not the same test. Three
//  classes of sequence satisfy the continuation-byte rule without being valid
//  UTF-8, and all three are rejected here:
//
//    * encoded surrogates   ED A0 80 .. ED BF BF   (U+D800 .. U+DFFF)
//    * overlong encodings   E0 80 .. E0 9F, F0 80 .. F0 8F
//    * beyond U+10FFFF      F4 90 .. F4 BF
// ============================================================================

#include <string>

namespace utf8 {

// True only for strictly valid UTF-8: no surrogates, no overlong forms,
// nothing beyond U+10FFFF, no truncated sequences.
bool isWellFormed(const std::string& s);

// Replace every sequence that is not well-formed with U+FFFD. Readable text
// survives, and the result is guaranteed well-formed, so it can be embedded in
// a JSON string literal without ever producing a lone surrogate escape.
std::string sanitize(const std::string& s);

}  // namespace utf8
