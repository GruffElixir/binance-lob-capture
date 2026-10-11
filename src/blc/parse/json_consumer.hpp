#pragma once

#include <simdjson.h>
#include <string_view>
#include <utility>

namespace blc {

inline constexpr int kMaxJsonDepth = 128;

// True if `token` (a raw JSON number token, possibly with trailing whitespace) matches the JSON
// number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
inline bool is_json_number(std::string_view token) noexcept {
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\n' ||
                              token.back() == '\r')) {
        token.remove_suffix(1);
    }
    std::size_t i = 0;
    const std::size_t n = token.size();
    const auto digit = [&](std::size_t k) { return k < n && token[k] >= '0' && token[k] <= '9'; };
    if (i < n && token[i] == '-') {
        ++i;
    }
    if (!digit(i)) {
        return false;
    }
    if (token[i] == '0') {
        ++i;
    } else {
        while (digit(i)) {
            ++i;
        }
    }
    if (i < n && token[i] == '.') {
        ++i;
        if (!digit(i)) {
            return false;
        }
        while (digit(i)) {
            ++i;
        }
    }
    if (i < n && (token[i] == 'e' || token[i] == 'E')) {
        ++i;
        if (i < n && (token[i] == '+' || token[i] == '-')) {
            ++i;
        }
        if (!digit(i)) {
            return false;
        }
        while (digit(i)) {
            ++i;
        }
    }
    return i == n;
}

// Visits and validates one JSON value completely, without allocating: every object field and
// array element is visited, strings are unescaped into the parser's buffer, literals are checked
// and numbers are validated against the grammar without numeric conversion (so huge numbers in
// fields we ignore are accepted). Nothing is skipped unvalidated. Depth is bounded.
inline simdjson::error_code consume_json_value(simdjson::ondemand::value& v,
                                               int depth = 0) noexcept {
    using simdjson::ondemand::json_type;
    if (depth > kMaxJsonDepth) {
        return simdjson::DEPTH_ERROR;
    }
    json_type t{};
    if (auto e = v.type().get(t)) {
        return e;
    }
    switch (t) {
        case json_type::object: {
            simdjson::ondemand::object obj;
            if (auto e = v.get_object().get(obj)) {
                return e;
            }
            for (auto field_result : obj) {
                simdjson::ondemand::field field;
                if (auto e = std::move(field_result).get(field)) {
                    return e;
                }
                std::string_view key;
                if (auto e = field.unescaped_key().get(key)) {
                    return e;
                }
                if (auto e = consume_json_value(field.value(), depth + 1)) {
                    return e;
                }
            }
            return simdjson::SUCCESS;
        }
        case json_type::array: {
            simdjson::ondemand::array arr;
            if (auto e = v.get_array().get(arr)) {
                return e;
            }
            for (auto element_result : arr) {
                simdjson::ondemand::value element;
                if (auto e = std::move(element_result).get(element)) {
                    return e;
                }
                if (auto e = consume_json_value(element, depth + 1)) {
                    return e;
                }
            }
            return simdjson::SUCCESS;
        }
        case json_type::string: {
            std::string_view s;
            return v.get_string().get(s);
        }
        case json_type::boolean: {
            bool b = false;
            return v.get_bool().get(b);
        }
        case json_type::null: {
            bool is_null = false;
            if (auto e = v.is_null().get(is_null)) {
                return e;
            }
            return is_null ? simdjson::SUCCESS : simdjson::N_ATOM_ERROR;
        }
        case json_type::number:
            return is_json_number(v.raw_json_token()) ? simdjson::SUCCESS : simdjson::NUMBER_ERROR;
        default:
            return simdjson::TAPE_ERROR;
    }
}

}  // namespace blc
