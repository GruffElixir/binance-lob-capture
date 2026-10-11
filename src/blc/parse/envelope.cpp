#include "blc/parse/envelope.hpp"

#include "blc/parse/json_consumer.hpp"

namespace blc {
namespace {

constexpr std::string_view kPrefix = R"({"stream":")";
constexpr std::string_view kDataKey = R"(","data":)";

// Returns the index one past the object starting at `begin` ('{'), or npos. Tracks strings,
// escapes and nesting of both objects and arrays.
std::size_t end_of_object(std::string_view s, std::size_t begin) noexcept {
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = begin; i < s.size(); ++i) {
        const char c = s[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // skip the escaped character
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) {
                return c == '}' ? i + 1 : std::string_view::npos;
            }
        }
    }
    return std::string_view::npos;
}

std::string_view trim_trailing(std::string_view s) noexcept {
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

bool EnvelopeSplitter::split(std::string_view frame, Envelope& out) noexcept {
    // Full syntax validation first: a frame with invalid JSON anywhere is rejected (a loss).
    if (!workspace_.object(frame)) {
        return false;
    }

    // Fast path: exactly {"stream":"<name>","data":{...}} with nothing else.
    if (frame.substr(0, kPrefix.size()) == kPrefix) {
        const std::size_t name_begin = kPrefix.size();
        const std::size_t name_end = frame.find('"', name_begin);
        if (name_end != std::string_view::npos &&
            frame.substr(name_begin, name_end - name_begin).find('\\') == std::string_view::npos &&
            frame.substr(name_end, kDataKey.size()) == kDataKey) {
            const std::size_t data_begin = name_end + kDataKey.size();
            if (data_begin < frame.size() && frame[data_begin] == '{') {
                const std::size_t data_end = end_of_object(frame, data_begin);
                if (data_end != std::string_view::npos && data_end + 1 == frame.size() &&
                    frame[data_end] == '}') {
                    out.stream = frame.substr(name_begin, name_end - name_begin);
                    out.data = frame.substr(data_begin, data_end - data_begin);
                    return true;
                }
            }
        }
    }
    ++fallbacks_;
    return fallback(frame, out);
}

bool EnvelopeSplitter::fallback(std::string_view frame, Envelope& out) noexcept {
    simdjson::ondemand::document doc;
    if (workspace_.iterate(frame, doc) != simdjson::SUCCESS) {
        return false;
    }
    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj) != simdjson::SUCCESS) {
        return false;
    }
    int streams = 0;
    int datas = 0;
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
        if (key == "stream") {
            if (++streams > 1 || value.get_string().get(out.stream) != simdjson::SUCCESS) {
                return false;
            }
        } else if (key == "data") {
            simdjson::ondemand::json_type t{};
            if (++datas > 1 || value.type().get(t) != simdjson::SUCCESS ||
                t != simdjson::ondemand::json_type::object) {
                return false;
            }
            std::string_view raw;
            if (value.raw_json().get(raw) != simdjson::SUCCESS) {
                return false;
            }
            out.data = trim_trailing(raw);
        } else {
            return false;  // the combined-stream envelope has exactly two fields
        }
    }
    return streams == 1 && datas == 1 && doc.at_end();
}

}  // namespace blc
