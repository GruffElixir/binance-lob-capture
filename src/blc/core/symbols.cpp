#include "blc/core/symbols.hpp"

#include "blc/core/errors.hpp"
#include "blc/core/instrument_id.hpp"

#include <algorithm>

namespace blc {
namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

std::vector<SymbolSpec> parse_symbol_list(Venue venue, std::string_view list) {
    std::vector<SymbolSpec> out;
    std::size_t position = 1;
    for (;;) {
        const std::size_t comma = list.find(',');
        const std::string_view raw = trim(list.substr(0, comma));
        if (raw.empty()) {
            throw UsageError("--symbols: empty symbol at position " + std::to_string(position) +
                             " (lists are comma-separated, e.g. BTCUSDT,ETHUSDT)");
        }
        SymbolSpec spec;
        for (const char c : raw) {
            const bool lower = c >= 'a' && c <= 'z';
            const bool upper = c >= 'A' && c <= 'Z';
            const bool digit = c >= '0' && c <= '9';
            if (!lower && !upper && !digit) {
                throw UsageError("--symbols: invalid symbol '" + std::string(raw) +
                                 "' (only letters and digits are allowed)");
            }
            spec.upper.push_back(lower ? static_cast<char>(c - 'a' + 'A') : c);
            spec.lower.push_back(upper ? static_cast<char>(c - 'A' + 'a') : c);
        }
        if (spec.upper.size() < 2 || spec.upper.size() > 20) {
            throw UsageError("--symbols: invalid symbol '" + std::string(raw) +
                             "' (must be 2-20 characters)");
        }
        for (const auto& existing : out) {
            if (existing.upper == spec.upper) {
                throw UsageError("--symbols: duplicate symbol " + spec.upper);
            }
        }
        spec.id = instrument_id(venue, spec.upper);
        for (const auto& existing : out) {
            if (existing.id == spec.id) {
                throw UsageError("--symbols: " + existing.upper + " and " + spec.upper +
                                 " map to the same instrument id");
            }
        }
        out.push_back(std::move(spec));
        if (out.size() > kMaxSymbols) {
            throw UsageError("--symbols: at most " + std::to_string(kMaxSymbols) +
                             " symbols are supported");
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
        ++position;
    }
    return out;
}

}  // namespace blc
