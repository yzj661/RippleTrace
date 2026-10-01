// ============================================================
//  RippleTrace - json.h
//  A small, strict JSON reader.
//
//  Why hand-rolled: the LLM layer must parse model responses, and the
//  project's zero-dependency property is load-bearing (the whole thing
//  builds with a bare C++17 compiler). Regex-based field extraction was
//  tried first and broke on escaped quotes and nested objects, so this is
//  a real parser with real tests instead.
// ============================================================
#pragma once

#include <map>
#include <string>
#include <vector>

namespace rippletrace {
namespace json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    static Value makeNull();
    static Value makeBool(bool b);
    static Value makeNumber(double d);
    static Value makeString(std::string s);
    static Value makeArray();
    static Value makeObject();

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool fallback = false) const;
    double asNumber(double fallback = 0.0) const;
    const std::string& asString() const;
    std::string asString(const std::string& fallback) const;

    // Object access. Returns a shared null Value when absent.
    bool has(const std::string& key) const;
    const Value& operator[](const std::string& key) const;

    // Array access.
    size_t size() const;
    const Value& at(size_t i) const;
    const std::vector<Value>& items() const { return array_; }
    const std::map<std::string, Value>& fields() const { return object_; }

    // Mutation, for building values programmatically.
    void set(const std::string& key, Value v);
    void push(Value v);

    // Look up a dotted path: "choices.0.message.content".
    // Returns a shared null Value when any step is missing.
    const Value& path(const std::string& dotted) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> array_;
    std::map<std::string, Value> object_;
};

// Parse `text` as JSON. On failure returns false and fills `error` with a
// message that includes the byte offset, so failures are diagnosable.
bool parse(const std::string& text, Value& out, std::string& error);

// Convenience: parse and return a null Value on failure.
Value parseOrNull(const std::string& text);

// Serialize. Compact by default, pass pretty=true for readable output.
std::string serialize(const Value& value, bool pretty = false);

// Escape a raw string into a JSON string literal (with quotes).
std::string quote(const std::string& raw);

}  // namespace json
}  // namespace rippletrace
