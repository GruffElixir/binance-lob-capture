#include "blc/parse/json_workspace.hpp"

#include "blc/parse/json_consumer.hpp"

#include <charconv>
#include <cstring>
#include <new>

namespace blc {
namespace {

// Strict JSON integer token -> int64 (no fraction or exponent; overflow rejected).
std::optional<std::int64_t> integer_token(std::string_view token) noexcept {
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\n' ||
                              token.back() == '\r')) {
        token.remove_suffix(1);
    }
    if (!is_json_number(token) || token.find_first_of(".eE") != std::string_view::npos) {
        return std::nullopt;
    }
    std::int64_t v = 0;
    const auto res = std::from_chars(token.data(), token.data() + token.size(), v);
    if (res.ec != std::errc{} || res.ptr != token.data() + token.size()) {
        return std::nullopt;
    }
    return v;
}

}  // namespace

JsonWorkspace::JsonWorkspace() : scratch_(kMaxNetworkMessageBytes + simdjson::SIMDJSON_PADDING) {
    // Pre-allocate for the largest accepted message so iterate() never allocates.
    if (parser_.allocate(kMaxNetworkMessageBytes, kMaxJsonDepth + 2) != simdjson::SUCCESS) {
        throw std::bad_alloc();
    }
}

simdjson::error_code JsonWorkspace::iterate(std::string_view text,
                                            simdjson::ondemand::document& doc) noexcept {
    if (text.size() > kMaxNetworkMessageBytes) {
        return simdjson::CAPACITY;
    }
    std::memcpy(scratch_.data(), text.data(), text.size());
    std::memset(scratch_.data() + text.size(), 0, simdjson::SIMDJSON_PADDING);
    return parser_
        .iterate(simdjson::padded_string_view(scratch_.data(), text.size(), scratch_.size()))
        .get(doc);
}

bool JsonWorkspace::object(std::string_view text,
                           std::optional<std::int64_t>* root_error_code) noexcept {
    if (root_error_code != nullptr) {
        root_error_code->reset();
    }
    simdjson::ondemand::document doc;
    if (iterate(text, doc) != simdjson::SUCCESS) {
        return false;
    }
    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj) != simdjson::SUCCESS) {
        return false;
    }
    int code_count = 0;
    std::optional<std::int64_t> code;
    for (auto field_result : obj) {
        simdjson::ondemand::field field;
        if (std::move(field_result).get(field) != simdjson::SUCCESS) {
            return false;
        }
        std::string_view key;
        if (field.unescaped_key().get(key) != simdjson::SUCCESS) {
            return false;
        }
        simdjson::ondemand::value& value = field.value();
        if (root_error_code != nullptr && key == "code") {
            ++code_count;
            simdjson::ondemand::json_type t{};
            if (value.type().get(t) != simdjson::SUCCESS) {
                return false;
            }
            if (t == simdjson::ondemand::json_type::number) {
                code = integer_token(value.raw_json_token());
            }
        }
        if (consume_json_value(value, 1) != simdjson::SUCCESS) {
            return false;
        }
    }
    if (!doc.at_end()) {
        return false;
    }
    if (root_error_code != nullptr && code_count == 1) {
        *root_error_code = code;
    }
    return true;
}

}  // namespace blc
