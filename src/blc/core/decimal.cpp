#include "blc/core/decimal.hpp"

#include <array>
#include <cstddef>

namespace blc {
namespace {

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr int kScaleDigits = 8;
constexpr std::array<std::int64_t, kScaleDigits + 1> kPow10 = {
    1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000};

}  // namespace

DecimalError parse_e8(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty()) {
        return DecimalError::Empty;
    }

    // Grammar check first, so a malformed string is always InvalidChar regardless of length.
    const std::size_t dot = text.find('.');
    const std::string_view int_part = text.substr(0, dot);
    const std::string_view frac_part =
        dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (int_part.empty() || (dot != std::string_view::npos && frac_part.empty())) {
        return DecimalError::InvalidChar;
    }
    for (const char c : int_part) {
        if (!is_digit(c)) {
            return DecimalError::InvalidChar;
        }
    }
    for (const char c : frac_part) {
        if (!is_digit(c)) {
            return DecimalError::InvalidChar;
        }
    }

    // Digits past the 8th decimal must be zero: the value is never rounded.
    for (std::size_t i = kScaleDigits; i < frac_part.size(); ++i) {
        if (frac_part[i] != '0') {
            return DecimalError::TooPrecise;
        }
    }

    std::int64_t whole = 0;
    for (const char c : int_part) {
        if (__builtin_mul_overflow(whole, std::int64_t{10}, &whole) ||
            __builtin_add_overflow(whole, std::int64_t{c - '0'}, &whole)) {
            return DecimalError::Overflow;
        }
    }

    std::int64_t frac = 0;
    const std::size_t used = frac_part.size() < kScaleDigits ? frac_part.size() : kScaleDigits;
    for (std::size_t i = 0; i < used; ++i) {
        frac = frac * 10 + (frac_part[i] - '0');
    }
    frac *= kPow10[static_cast<std::size_t>(kScaleDigits) - used];

    std::int64_t scaled = 0;
    if (__builtin_mul_overflow(whole, kPow10[kScaleDigits], &scaled) ||
        __builtin_add_overflow(scaled, frac, &scaled)) {
        return DecimalError::Overflow;
    }
    out = scaled;
    return DecimalError::Ok;
}

}  // namespace blc
