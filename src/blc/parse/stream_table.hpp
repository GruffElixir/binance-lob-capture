#pragma once

#include "blc/core/symbols.hpp"
#include "blc/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blc {

struct StreamEntry {
    std::uint16_t symbol_index;
    StreamKind kind;
};

// Maps combined-stream names ("btcusdt@depth@100ms", ...) of one shard to (symbol, kind).
class StreamTable {
public:
    StreamTable(const std::vector<SymbolSpec>& shard_symbols, std::uint16_t first_symbol_index);

    std::optional<StreamEntry> find(std::string_view stream_name) const noexcept;

    // Appends "<sym>@depth@100ms/<sym>@depth5@100ms/<sym>@trade/..." in symbol order.
    void append_stream_list(std::string& out) const;

private:
    std::vector<std::pair<std::string, StreamEntry>> sorted_;
    std::vector<std::string> ordered_names_;
};

}  // namespace blc
