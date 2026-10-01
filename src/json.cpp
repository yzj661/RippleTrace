// ============================================================
//  RippleTrace - json.cpp
// ============================================================
#include "json.h"
#include "utf8.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace rippletrace {
namespace json {

namespace {

Value g_null;

const Value& nullValue() { return g_null; }

}  // namespace

Value Value::makeNull() { Value v; v.type_ = Type::Null; return v; }
Value Value::makeBool(bool b) { Value v; v.type_ = Type::Bool; v.bool_ = b; return v; }
Value Value::makeNumber(double d) { Value v; v.type_ = Type::Number; v.number_ = d; return v; }
Value Value::makeString(std::string s) {
    Value v; v.type_ = Type::String; v.string_ = std::move(s); return v;
}
Value Value::makeArray() { Value v; v.type_ = Type::Array; return v; }
Value Value::makeObject() { Value v; v.type_ = Type::Object; return v; }

bool Value::asBool(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

double Value::asNumber(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

const std::string& Value::asString() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

std::string Value::asString(const std::string& fallback) const {
    return type_ == Type::String ? string_ : fallback;
}

bool Value::has(const std::string& key) const {
    return type_ == Type::Object && object_.count(key) > 0;
}

const Value& Value::operator[](const std::string& key) const {
    if (type_ != Type::Object) return nullValue();
    auto it = object_.find(key);
    return it == object_.end() ? nullValue() : it->second;
}

size_t Value::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

const Value& Value::at(size_t i) const {
    if (type_ != Type::Array || i >= array_.size()) return nullValue();
    return array_[i];
}

void Value::set(const std::string& key, Value v) {
    type_ = Type::Object;
    object_[key] = std::move(v);
}

void Value::push(Value v) {
    type_ = Type::Array;
    array_.push_back(std::move(v));
}

const Value& Value::path(const std::string& dotted) const {
    const Value* cur = this;
    size_t start = 0;
    while (start <= dotted.size()) {
        size_t dot = dotted.find('.', start);
        std::string key = dotted.substr(start, dot == std::string::npos ? std::string::npos
                                                                       : dot - start);
        if (cur->type_ == Type::Object) {
            auto it = cur->object_.find(key);
            if (it == cur->object_.end()) return nullValue();
            cur = &it->second;
        } else if (cur->type_ == Type::Array) {
            char* end = nullptr;
            long idx = std::strtol(key.c_str(), &end, 10);
            if (end == key.c_str() || idx < 0 ||
                static_cast<size_t>(idx) >= cur->array_.size()) {
                return nullValue();
            }
            cur = &cur->array_[static_cast<size_t>(idx)];
        } else {
            return nullValue();
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return *cur;
}

// ------------------------------------------------------------
//  parser
// ------------------------------------------------------------
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;

    explicit Parser(const std::string& text) : s(text) {}

    void skipWs() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
            break;
        }
    }

    bool fail(const std::string& msg) {
        if (error.empty()) {
            error = msg + " at offset " + std::to_string(i);
        }
        return false;
    }

    bool parseValue(Value& out) {
        skipWs();
        if (i >= s.size()) return fail("unexpected end of input");
        char c = s[i];
        switch (c) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Value::makeString(std::move(str));
                return true;
            }
            case 't':
                if (s.compare(i, 4, "true") == 0) { i += 4; out = Value::makeBool(true); return true; }
                return fail("invalid literal");
            case 'f':
                if (s.compare(i, 5, "false") == 0) { i += 5; out = Value::makeBool(false); return true; }
                return fail("invalid literal");
            case 'n':
                if (s.compare(i, 4, "null") == 0) { i += 4; out = Value::makeNull(); return true; }
                return fail("invalid literal");
            default:
                return parseNumber(out);
        }
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected '\"'");
        ++i;
        out.clear();
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') { ++i; return true; }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return fail("dangling escape");
                char e = s[i++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("truncated \\u escape");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("bad hex in \\u escape");
                        }
                        i += 4;
                        // Encode as UTF-8. Surrogate halves are passed through as
                        // replacement-free bytes, which is adequate for prompts.
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("unknown escape");
                }
                continue;
            }
            out.push_back(static_cast<char>(c));
            ++i;
        }
        return fail("unterminated string");
    }

    bool parseNumber(Value& out) {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool digits = false;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; digits = true; }
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; digits = true; }
        }
        if (digits && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        }
        if (!digits) return fail("expected a value");
        double d = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        out = Value::makeNumber(d);
        return true;
    }

    bool parseArray(Value& out) {
        ++i;   // '['
        out = Value::makeArray();
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        for (;;) {
            Value item;
            if (!parseValue(item)) return false;
            out.push(std::move(item));
            skipWs();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Value& out) {
        ++i;   // '{'
        out = Value::makeObject();
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            ++i;
            Value val;
            if (!parseValue(val)) return false;
            out.set(key, std::move(val));
            skipWs();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return fail("expected ',' or '}'");
        }
    }
};

void serializeInto(const Value& v, std::ostringstream& o, bool pretty, int indent);

void writeIndent(std::ostringstream& o, int n) {
    for (int k = 0; k < n; ++k) o << "  ";
}

void serializeInto(const Value& v, std::ostringstream& o, bool pretty, int indent) {
    switch (v.type()) {
        case Value::Type::Null:   o << "null"; break;
        case Value::Type::Bool:   o << (v.asBool() ? "true" : "false"); break;
        case Value::Type::Number: {
            double d = v.asNumber();
            if (d == std::floor(d) && std::fabs(d) < 1e15) {
                o << static_cast<long long>(d);
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.10g", d);
                o << buf;
            }
            break;
        }
        case Value::Type::String: o << quote(v.asString()); break;
        case Value::Type::Array: {
            const auto& items = v.items();
            if (items.empty()) { o << "[]"; break; }
            o << "[";
            for (size_t k = 0; k < items.size(); ++k) {
                if (k) o << ",";
                if (pretty) { o << "\n"; writeIndent(o, indent + 1); }
                serializeInto(items[k], o, pretty, indent + 1);
            }
            if (pretty) { o << "\n"; writeIndent(o, indent); }
            o << "]";
            break;
        }
        case Value::Type::Object: {
            const auto& fields = v.fields();
            if (fields.empty()) { o << "{}"; break; }
            o << "{";
            bool first = true;
            for (const auto& kv : fields) {
                if (!first) o << ",";
                first = false;
                if (pretty) { o << "\n"; writeIndent(o, indent + 1); }
                o << quote(kv.first) << (pretty ? ": " : ":");
                serializeInto(kv.second, o, pretty, indent + 1);
            }
            if (pretty) { o << "\n"; writeIndent(o, indent); }
            o << "}";
            break;
        }
    }
}

}  // namespace

bool parse(const std::string& text, Value& out, std::string& error) {
    Parser p(text);
    if (!p.parseValue(out)) {
        error = p.error.empty() ? "parse error" : p.error;
        return false;
    }
    p.skipWs();
    if (p.i != text.size()) {
        error = "trailing data at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

Value parseOrNull(const std::string& text) {
    Value v;
    std::string err;
    if (!parse(text, v, err)) return Value::makeNull();
    return v;
}

std::string serialize(const Value& value, bool pretty) {
    std::ostringstream o;
    serializeInto(value, o, pretty, 0);
    return o.str();
}

// Drop or replace invalid UTF-8 so the value can be encoded at all.
//
// A byte >= 0x80 that is not part of a valid sequence cannot be represented as
// a JSON escape. Emitting it as \uD800-style halves produces a lone surrogate,
// which strict servers reject outright:
//     HTTP 400 ... lone leading surrogate in hex escape
//
// The actual rules live in utf8.h so that the reader and the encoder cannot
// drift apart; both used to accept encoded surrogates, and that is how a WTF-8
// byte pair from a GBK source file reached the model prompt.
std::string sanitizeUtf8(const std::string& in) {
    return utf8::sanitize(in);
}

std::string quote(const std::string& rawIn) {
    const std::string raw = sanitizeUtf8(rawIn);
    std::string out;
    out.reserve(raw.size() + 8);
    out.push_back('"');
    for (unsigned char c : raw) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace json
}  // namespace rippletrace
