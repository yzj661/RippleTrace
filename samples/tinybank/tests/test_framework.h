// tinybank - micro test framework
// Layer: test-only. Each test is a real function so a static analyzer can see
// the test -> code edges and reason about test coverage of a change.
#pragma once

#include <iostream>
#include <string>

namespace tinybank {
namespace test {

inline int& failures() {
    static int value = 0;
    return value;
}

inline int& checks() {
    static int value = 0;
    return value;
}

inline void report_check(bool condition,
                         const char* expression,
                         const char* file,
                         int line,
                         const std::string& context) {
    ++checks();
    if (!condition) {
        ++failures();
        std::cerr << "  FAIL " << file << ":" << line << "  " << expression;
        if (!context.empty()) std::cerr << "   [" << context << "]";
        std::cerr << "\n";
    }
}

inline void report_case(const std::string& name, int failuresBefore) {
    const int delta = failures() - failuresBefore;
    std::cout << (delta == 0 ? "  ok   " : "  FAIL ") << name;
    if (delta != 0) std::cout << "  (" << delta << " failing check(s))";
    std::cout << "\n";
}

}  // namespace test
}  // namespace tinybank

#define TB_CHECK(expr)                                                        \
    ::tinybank::test::report_check(static_cast<bool>(expr), #expr, __FILE__,  \
                                   __LINE__, std::string())

#define TB_CHECK_EQ(actual, expected)                                         \
    ::tinybank::test::report_check((actual) == (expected),                    \
                                   #actual " == " #expected, __FILE__,        \
                                   __LINE__, std::string())

#define TB_CHECK_MSG(expr, msg)                                               \
    ::tinybank::test::report_check(static_cast<bool>(expr), #expr, __FILE__,  \
                                   __LINE__, std::string(msg))

#define TB_CASE(name)                                                          \
    const int tb_failures_before_ = ::tinybank::test::failures();              \
    ::tinybank::test::report_case(name, tb_failures_before_)
