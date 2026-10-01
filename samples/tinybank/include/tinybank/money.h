// tinybank - fixed-point money type
// Layer: domain primitive. Shared by account, transfer and reporting paths.
#pragma once

#include <cstdint>
#include <string>

namespace tinybank {

class Money {
public:
    Money();
    explicit Money(std::int64_t cents);

    static Money from_cents(std::int64_t cents);
    static bool parse(const std::string& text, Money& out);

    std::int64_t cents() const { return cents_; }
    bool is_negative() const { return cents_ < 0; }

    std::string format() const;
    std::string format_with_currency(const std::string& symbol) const;

    Money operator+(const Money& other) const;
    Money operator-(const Money& other) const;
    bool operator==(const Money& other) const;

private:
    std::int64_t cents_;
};

}  // namespace tinybank
