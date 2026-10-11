#include "blc/parse/stream_table.hpp"

#include <algorithm>

namespace blc {

StreamTable::StreamTable(const std::vector<SymbolSpec>& shard_symbols,
                         std::uint16_t first_symbol_index) {
    static constexpr std::pair<std::string_view, StreamKind> kSuffixes[] = {
        {"@depth@100ms", StreamKind::DepthDiff},
        {"@depth5@100ms", StreamKind::Depth5},
        {"@trade", StreamKind::Trade},
    };
    std::uint16_t index = first_symbol_index;
    for (const auto& symbol : shard_symbols) {
        for (const auto& [suffix, kind] : kSuffixes) {
            std::string name = symbol.lower + std::string(suffix);
            ordered_names_.push_back(name);
            sorted_.push_back({std::move(name), StreamEntry{index, kind}});
        }
        ++index;
    }
    std::sort(sorted_.begin(), sorted_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

std::optional<StreamEntry> StreamTable::find(std::string_view stream_name) const noexcept {
    const auto it = std::lower_bound(
        sorted_.begin(), sorted_.end(), stream_name,
        [](const auto& entry, std::string_view name) { return entry.first < name; });
    if (it == sorted_.end() || it->first != stream_name) {
        return std::nullopt;
    }
    return it->second;
}

void StreamTable::append_stream_list(std::string& out) const {
    bool first = true;
    for (const auto& name : ordered_names_) {
        if (!first) {
            out.push_back('/');
        }
        out += name;
        first = false;
    }
}

}  // namespace blc
