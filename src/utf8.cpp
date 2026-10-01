#include "utf8.h"

namespace utf8 {

namespace {

int sequenceLength(unsigned char lead) {
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 0;   // continuation byte used as a lead, or 0xC0/0xC1/0xF5..0xFF
}

// Bounds for the byte after the lead. The narrowed ranges are precisely what
// excludes overlong forms, encoded surrogates and code points past U+10FFFF.
bool secondByteOk(unsigned char lead, unsigned char second) {
    if (lead == 0xE0) return second >= 0xA0 && second <= 0xBF;  // no overlong 3-byte
    if (lead == 0xED) return second >= 0x80 && second <= 0x9F;  // not U+D800..U+DFFF
    if (lead == 0xF0) return second >= 0x90 && second <= 0xBF;  // no overlong 4-byte
    if (lead == 0xF4) return second >= 0x80 && second <= 0x8F;  // not past U+10FFFF
    return second >= 0x80 && second <= 0xBF;
}

bool continuationsOk(const std::string& s, size_t start, int len) {
    if (start + static_cast<size_t>(len) > s.size()) return false;
    if (!secondByteOk(static_cast<unsigned char>(s[start]),
                      static_cast<unsigned char>(s[start + 1]))) {
        return false;
    }
    for (int k = 2; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[start + k]);
        if (cc < 0x80 || cc > 0xBF) return false;
    }
    return true;
}

}  // namespace

bool isWellFormed(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { ++i; continue; }
        const int len = sequenceLength(c);
        if (len == 0) return false;
        if (!continuationsOk(s, i, len)) return false;
        i += static_cast<size_t>(len);
    }
    return true;
}

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        const int len = sequenceLength(c);
        if (len > 0 && i + static_cast<size_t>(len) <= s.size()) {
            bool continuations = true;
            for (int k = 1; k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if (cc < 0x80 || cc > 0xBF) { continuations = false; break; }
            }
            if (continuations) {
                // The whole sequence is present. Whether it is a valid code
                // point or an overlong / surrogate / out-of-range encoding, it
                // is one ill-formed subsequence, so it gets one replacement
                // character rather than one per byte.
                if (secondByteOk(c, static_cast<unsigned char>(s[i + 1]))) {
                    out.append(s, i, static_cast<size_t>(len));
                } else {
                    out += "\xEF\xBF\xBD";
                }
                i += static_cast<size_t>(len);
                continue;
            }
        }
        // Truncated, or a lead byte followed by something that cannot continue
        // it. Advance one byte so the scan always progresses and the bytes
        // after the bad one still get a chance to be recognised.
        out += "\xEF\xBF\xBD";   // U+FFFD
        ++i;
    }
    return out;
}

}  // namespace utf8
