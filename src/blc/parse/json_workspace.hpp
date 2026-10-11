#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <simdjson.h>
#include <string_view>
#include <vector>

namespace blc {

inline constexpr std::size_t kMaxNetworkMessageBytes = std::size_t{1} << 20;

// One padded 1 MiB scratch buffer plus one pre-allocated On-Demand parser, used serially on the
// io thread for envelope validation/fallback and REST body validation (design §6.2, §9).
class JsonWorkspace {
public:
    JsonWorkspace();

    // Copies `text` into the padded scratch and validates it completely as one JSON object
    // (nothing may follow it). When `root_error_code` is given, a unique integer root "code"
    // field is decoded into it; it stays empty when absent, duplicated or non-integer.
    bool object(std::string_view text,
                std::optional<std::int64_t>* root_error_code = nullptr) noexcept;

    // Copies `text` into the scratch (if it fits) and starts iterating it. The document and
    // every view obtained from it stay valid until the next call on this workspace.
    simdjson::error_code iterate(std::string_view text, simdjson::ondemand::document& doc) noexcept;

    std::size_t capacity() const noexcept { return kMaxNetworkMessageBytes; }

private:
    std::vector<char> scratch_;
    simdjson::ondemand::parser parser_;
};

}  // namespace blc
