// tinybank - fixed-point money type
#include "tinybank/money.h"

#include "tinybank/strings.h"

#include <cstdlib>

namespace tinybank {

Money::Money() : cents_(0) {}

Money::Money(std::int64_t cents) : cents_(cents) {}

Money Money::from_cents(std::int64_t cents) { return Money(cents); }

bool Money::parse(const std::string& text, Money& out) {
    const std::string t = trim(text);
    if (t.empty()) return false;

    bool negative = false;
    std::size_t i = 0;
    if (t[0] == '-') { negative = true; i = 1; }
    else if (t[0] == '+') { i = 1; }

    std::int64_t whole = 0;
    bool sawDigit = false;
    for (; i < t.size() && t[i] != '.'; ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        whole = whole * 10 + (t[i] - '0');
        sawDigit = true;
    }
    std::int64_t frac = 0;
    int fracDigits = 0;
    if (i < t.size() && t[i] == '.') {
        ++i;
        for (; i < t.size(); ++i) {
            if (t[i] < '0' || t[i] > '9') return false;
            if (fracDigits < 2) {
                frac = frac * 10 + (t[i] - '0');
                ++fracDigits;
            }
            sawDigit = true;
        }
    }
    if (!sawDigit) return false;
    while (fracDigits < 2) { frac *= 10; ++fracDigits; }

    std::int64_t total = whole * 100 + frac;
    out = Money(negative ? -total : total);
    return true;
}

std::string Money::format() const {
    const std::int64_t abs = cents_ < 0 ? -cents_ : cents_;
    const std::int64_t whole = abs / 100;
    const std::int64_t frac = abs % 100;
    return std::string(cents_ < 0 ? "-" : "") + std::to_string(whole) + "." +
           pad_left(std::to_string(frac), 2, '0');
}

std::string Money::format_with_currency(const std::string& symbol) const {
    return symbol + format();
}

Money Money::operator+(const Money& other) const {
    return Money(cents_ + other.cents_);
}

Money Money::operator-(const Money& other) const {
    return Money(cents_ - other.cents_);
}

bool Money::operator==(const Money& other) const {
    return cents_ == other.cents_;
}

}  // namespace tinybank
