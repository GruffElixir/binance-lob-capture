#include "blc/parse/payload_parser.hpp"

#include "blc/core/decimal.hpp"
#include "blc/parse/json_consumer.hpp"

#include <new>
#include <utility>

namespace blc {
namespace {

using simdjson::ondemand::value;

ParseStatus from_error(simdjson::error_code e) noexcept {
    return e == simdjson::DEPTH_ERROR ? ParseStatus::Oversized : ParseStatus::Malformed;
}

// Consumes (validates) a value whose content we do not use.
ParseStatus skip(value& v) noexcept {
    const auto e = consume_json_value(v, 1);
    return e == simdjson::SUCCESS ? ParseStatus::Ok : from_error(e);
}

ParseStatus read_u64(value& v, std::uint64_t& out) noexcept {
    return v.get_uint64().get(out) == simdjson::SUCCESS ? ParseStatus::Ok : ParseStatus::Malformed;
}

ParseStatus check_symbol(value& v, std::string_view expected) noexcept {
    std::string_view s;
    if (v.get_string().get(s) != simdjson::SUCCESS) {
        return ParseStatus::Malformed;
    }
    return s == expected ? ParseStatus::Ok : ParseStatus::SymbolMismatch;
}

enum class LevelRule {
    Diff,         // price > 0, qty >= 0, any order
    SnapshotBid,  // price > 0, qty > 0, strictly descending prices
    SnapshotAsk,  // price > 0, qty > 0, strictly ascending prices
};

// Reads [[price, qty], ...]. `push(level)` returns false when the destination is full; the
// status to report then is `when_full`.
template <class Push>
ParseStatus read_levels(value& v, LevelRule rule, ParseStatus when_full, Push&& push) noexcept {
    simdjson::ondemand::array levels;
    if (v.get_array().get(levels) != simdjson::SUCCESS) {
        return ParseStatus::Malformed;
    }
    bool have_previous = false;
    std::int64_t previous = 0;
    for (auto level_result : levels) {
        value level_value;
        if (std::move(level_result).get(level_value) != simdjson::SUCCESS) {
            return ParseStatus::Malformed;
        }
        simdjson::ondemand::array pair;
        if (level_value.get_array().get(pair) != simdjson::SUCCESS) {
            return ParseStatus::Malformed;
        }
        Level level{};
        int index = 0;
        for (auto element_result : pair) {
            value element;
            // NOLINTNEXTLINE(performance-move-const-arg): simdjson get() requires an rvalue
            if (std::move(element_result).get(element) != simdjson::SUCCESS || index >= 2) {
                return ParseStatus::Malformed;
            }
            std::string_view text;
            if (element.get_string().get(text) != simdjson::SUCCESS) {
                return ParseStatus::Malformed;
            }
            std::int64_t& target = index == 0 ? level.price : level.qty;
            if (parse_e8(text, target) != DecimalError::Ok) {
                return ParseStatus::BadDecimal;
            }
            ++index;
        }
        if (index != 2 || level.price <= 0) {
            return ParseStatus::Malformed;
        }
        if (rule != LevelRule::Diff) {
            if (level.qty <= 0) {
                return ParseStatus::Malformed;
            }
            if (have_previous && (rule == LevelRule::SnapshotBid ? level.price >= previous
                                                                 : level.price <= previous)) {
                return ParseStatus::Malformed;
            }
        }
        have_previous = true;
        previous = level.price;
        if (!push(level)) {
            return when_full;
        }
    }
    return ParseStatus::Ok;
}

// Iterates the root object, calling on_field(key, value) for every field. Ensures the payload
// fits, the root is an object and nothing follows it.
template <class OnField>
ParseStatus visit_root(simdjson::ondemand::parser& parser, PaddedView payload,
                       OnField&& on_field) noexcept {
    if (payload.size > kMaxPayloadBytes || payload.capacity < payload.size) {
        return ParseStatus::Oversized;
    }
    simdjson::ondemand::document doc;
    auto e =
        parser.iterate(simdjson::padded_string_view(payload.data, payload.size, payload.capacity))
            .get(doc);
    if (e != simdjson::SUCCESS) {
        return from_error(e);
    }
    simdjson::ondemand::object obj;
    if ((e = doc.get_object().get(obj)) != simdjson::SUCCESS) {
        return from_error(e);
    }
    for (auto field_result : obj) {
        simdjson::ondemand::field field;
        if ((e = std::move(field_result).get(field)) != simdjson::SUCCESS) {
            return from_error(e);
        }
        std::string_view key;
        if (field.unescaped_key().get(key) != simdjson::SUCCESS) {
            return ParseStatus::Malformed;
        }
        const ParseStatus st = on_field(key, field.value());
        if (st != ParseStatus::Ok) {
            return st;
        }
    }
    return doc.at_end() ? ParseStatus::Ok : ParseStatus::Malformed;
}

// Records that a required key was seen; a second occurrence is malformed.
bool mark(unsigned& seen, unsigned bit) noexcept {
    if ((seen & bit) != 0) {
        return false;
    }
    seen |= bit;
    return true;
}

}  // namespace

PayloadParser::PayloadParser() {
    if (parser_.allocate(kMaxPayloadBytes, kMaxJsonDepth + 2) != simdjson::SUCCESS) {
        throw std::bad_alloc();
    }
}

DepthDiff PayloadParser::make_diff() {
    DepthDiff d;
    d.bids.reserve(kMaxDiffLevels);
    d.asks.reserve(kMaxDiffLevels);
    return d;
}

DepthSnapshot PayloadParser::make_snapshot() {
    DepthSnapshot s;
    s.bids.reserve(kMaxSnapshotLevelsPerSide);
    s.asks.reserve(kMaxSnapshotLevelsPerSide);
    return s;
}

ParseStatus PayloadParser::diff(Venue venue, PaddedView payload, std::string_view upper_symbol,
                                DepthDiff& out) noexcept {
    enum : unsigned { kU = 1, kLastU = 2, kPu = 4, kB = 8, kA = 16, kS = 32 };
    out.bids.clear();
    out.asks.clear();
    out.has_prev = false;
    unsigned seen = 0;
    std::size_t total = 0;
    const bool usdm = venue == Venue::Usdm;
    const auto push_to = [&](std::vector<Level>& side) {
        return [&](const Level& l) {
            if (++total > kMaxDiffLevels) {
                return false;
            }
            side.push_back(l);
            return true;
        };
    };
    ParseStatus const st = visit_root(parser_, payload, [&](std::string_view key, value& v) {
        if (key == "U") {
            return mark(seen, kU) ? read_u64(v, out.first_id) : ParseStatus::Malformed;
        }
        if (key == "u") {
            return mark(seen, kLastU) ? read_u64(v, out.last_id) : ParseStatus::Malformed;
        }
        if (key == "pu" && usdm) {
            return mark(seen, kPu) ? read_u64(v, out.prev_last_id) : ParseStatus::Malformed;
        }
        if (key == "b") {
            return mark(seen, kB)
                       ? read_levels(v, LevelRule::Diff, ParseStatus::Oversized, push_to(out.bids))
                       : ParseStatus::Malformed;
        }
        if (key == "a") {
            return mark(seen, kA)
                       ? read_levels(v, LevelRule::Diff, ParseStatus::Oversized, push_to(out.asks))
                       : ParseStatus::Malformed;
        }
        if (key == "s") {
            return mark(seen, kS) ? check_symbol(v, upper_symbol) : ParseStatus::Malformed;
        }
        return skip(v);
    });
    if (st != ParseStatus::Ok) {
        return st;
    }
    const unsigned required = kU | kLastU | kB | kA | kS | (usdm ? kPu : 0u);
    if ((seen & required) != required || out.first_id > out.last_id) {
        return ParseStatus::Malformed;
    }
    out.has_prev = usdm;
    return ParseStatus::Ok;
}

ParseStatus PayloadParser::depth5(Venue venue, PaddedView payload, std::string_view upper_symbol,
                                  Depth5& out) noexcept {
    enum : unsigned { kId = 1, kB = 2, kA = 4, kS = 8 };
    const bool usdm = venue == Venue::Usdm;
    const std::string_view id_key = usdm ? "u" : "lastUpdateId";
    const std::string_view bid_key = usdm ? "b" : "bids";
    const std::string_view ask_key = usdm ? "a" : "asks";
    out.n_bids = 0;
    out.n_asks = 0;
    unsigned seen = 0;
    const auto push_to = [](std::array<Level, 5>& side, std::uint8_t& n) {
        return [&side, &n](const Level& l) {
            if (n >= side.size()) {
                return false;
            }
            side[n++] = l;
            return true;
        };
    };
    ParseStatus const st = visit_root(parser_, payload, [&](std::string_view key, value& v) {
        if (key == id_key) {
            return mark(seen, kId) ? read_u64(v, out.id) : ParseStatus::Malformed;
        }
        if (key == bid_key) {
            return mark(seen, kB) ? read_levels(v, LevelRule::SnapshotBid, ParseStatus::Malformed,
                                                push_to(out.bids, out.n_bids))
                                  : ParseStatus::Malformed;
        }
        if (key == ask_key) {
            return mark(seen, kA) ? read_levels(v, LevelRule::SnapshotAsk, ParseStatus::Malformed,
                                                push_to(out.asks, out.n_asks))
                                  : ParseStatus::Malformed;
        }
        if (key == "s" && usdm) {
            return mark(seen, kS) ? check_symbol(v, upper_symbol) : ParseStatus::Malformed;
        }
        return skip(v);
    });
    if (st != ParseStatus::Ok) {
        return st;
    }
    const unsigned required = kId | kB | kA | (usdm ? kS : 0u);
    return (seen & required) == required ? ParseStatus::Ok : ParseStatus::Malformed;
}

ParseStatus PayloadParser::trade(Venue /*venue*/, PaddedView payload, std::string_view upper_symbol,
                                 Trade& out) noexcept {
    enum : unsigned { kM = 1, kS = 2 };
    unsigned seen = 0;
    ParseStatus const st = visit_root(parser_, payload, [&](std::string_view key, value& v) {
        if (key == "m") {
            if (!mark(seen, kM)) {
                return ParseStatus::Malformed;
            }
            return v.get_bool().get(out.buyer_is_maker) == simdjson::SUCCESS
                       ? ParseStatus::Ok
                       : ParseStatus::Malformed;
        }
        if (key == "s") {
            return mark(seen, kS) ? check_symbol(v, upper_symbol) : ParseStatus::Malformed;
        }
        return skip(v);
    });
    if (st != ParseStatus::Ok) {
        return st;
    }
    return seen == (kM | kS) ? ParseStatus::Ok : ParseStatus::Malformed;
}

ParseStatus PayloadParser::snapshot(Venue /*venue*/, PaddedView payload,
                                    DepthSnapshot& out) noexcept {
    enum : unsigned { kId = 1, kB = 2, kA = 4 };
    out.bids.clear();
    out.asks.clear();
    unsigned seen = 0;
    const auto push_to = [](std::vector<Level>& side) {
        return [&side](const Level& l) {
            if (side.size() >= kMaxSnapshotLevelsPerSide) {
                return false;
            }
            side.push_back(l);
            return true;
        };
    };
    ParseStatus const st = visit_root(parser_, payload, [&](std::string_view key, value& v) {
        if (key == "lastUpdateId") {
            return mark(seen, kId) ? read_u64(v, out.last_update_id) : ParseStatus::Malformed;
        }
        if (key == "bids") {
            return mark(seen, kB) ? read_levels(v, LevelRule::SnapshotBid, ParseStatus::Oversized,
                                                push_to(out.bids))
                                  : ParseStatus::Malformed;
        }
        if (key == "asks") {
            return mark(seen, kA) ? read_levels(v, LevelRule::SnapshotAsk, ParseStatus::Oversized,
                                                push_to(out.asks))
                                  : ParseStatus::Malformed;
        }
        return skip(v);
    });
    if (st != ParseStatus::Ok) {
        return st;
    }
    return seen == (kId | kB | kA) ? ParseStatus::Ok : ParseStatus::Malformed;
}

}  // namespace blc
