#include "support/payload_json.hpp"

#include <cstdlib>

namespace blc::test {

namespace {

std::string levels(const Level* data, std::size_t n) {
    std::string out = "[";
    for (std::size_t i = 0; i < n; ++i) {
        out += i == 0 ? "[\"" : ",[\"";
        out += e8(data[i].price);
        out += "\",\"";
        out += e8(data[i].qty);
        out += "\"]";
    }
    out += "]";
    return out;
}

std::string levels(const std::vector<Level>& v) {
    return levels(v.data(), v.size());
}

}  // namespace

std::string e8(std::int64_t scaled) {
    std::string frac = std::to_string(scaled % 100'000'000);
    frac.insert(0, 8 - frac.size(), '0');
    return std::to_string(scaled / 100'000'000) + "." + frac;
}

std::string diff_json(Venue venue, std::string_view symbol, const DepthDiff& d) {
    std::string out = R"({"e":"depthUpdate","E":1791730545214,)";
    if (venue == Venue::Usdm) {
        out += R"("T":1791730545210,)";
    }
    out += R"("s":")" + std::string(symbol) + R"(","U":)" + std::to_string(d.first_id) +
           R"(,"u":)" + std::to_string(d.last_id);
    if (venue == Venue::Usdm) {
        out += R"(,"pu":)" + std::to_string(d.prev_last_id);
    }
    out += R"(,"b":)" + levels(d.bids) + R"(,"a":)" + levels(d.asks) + "}";
    return out;
}

std::string depth5_json(Venue venue, std::string_view symbol, const Depth5& d5) {
    if (venue == Venue::Spot) {
        return R"({"lastUpdateId":)" + std::to_string(d5.id) + R"(,"bids":)" +
               levels(d5.bids.data(), d5.n_bids) + R"(,"asks":)" +
               levels(d5.asks.data(), d5.n_asks) + "}";
    }
    return R"({"e":"depthUpdate","E":1791730545214,"T":1791730545210,"s":")" + std::string(symbol) +
           R"(","U":)" + std::to_string(d5.id) + R"(,"u":)" + std::to_string(d5.id) + R"(,"pu":)" +
           std::to_string(d5.id - 1) + R"(,"b":)" + levels(d5.bids.data(), d5.n_bids) + R"(,"a":)" +
           levels(d5.asks.data(), d5.n_asks) + "}";
}

std::string trade_json(Venue venue, std::string_view symbol, bool buyer_is_maker,
                       std::uint64_t trade_id) {
    std::string out = R"({"e":"trade","E":1791730545480,)";
    if (venue == Venue::Usdm) {
        out += R"("T":1791730545479,)";
    }
    out += R"("s":")" + std::string(symbol) + R"(","t":)" + std::to_string(trade_id) +
           R"(,"p":"83150.09000000","q":"0.03615000",)";
    if (venue == Venue::Spot) {
        out += R"("T":1791730545480,)";
    }
    out += R"("m":)" + std::string(buyer_is_maker ? "true" : "false");
    if (venue == Venue::Spot) {
        out += R"(,"M":true)";
    } else {
        out += R"(,"X":"MARKET")";
    }
    out += "}";
    return out;
}

std::string snapshot_json(Venue venue, const DepthSnapshot& s) {
    std::string out = R"({"lastUpdateId":)" + std::to_string(s.last_update_id);
    if (venue == Venue::Usdm) {
        out += R"(,"E":1791730545214,"T":1791730545210)";
    }
    out += R"(,"bids":)" + levels(s.bids) + R"(,"asks":)" + levels(s.asks) + "}";
    return out;
}

DepthDiff make_diff(std::uint64_t U, std::uint64_t u, std::uint64_t pu, std::vector<Level> bids,
                    std::vector<Level> asks) {
    DepthDiff d;
    d.first_id = U;
    d.last_id = u;
    d.prev_last_id = pu;
    d.bids = std::move(bids);
    d.asks = std::move(asks);
    return d;
}

Depth5 make_depth5(std::uint64_t id, const std::vector<Level>& bids,
                   const std::vector<Level>& asks) {
    Depth5 d{};
    d.id = id;
    for (const Level& l : bids) {
        d.bids[d.n_bids++] = l;
    }
    for (const Level& l : asks) {
        d.asks[d.n_asks++] = l;
    }
    return d;
}

OwnedRecord::OwnedRecord(const RecordHeader& header, const std::string& payload)
    : header_(header), bytes_(payload.begin(), payload.end()), size_(payload.size()) {
    bytes_.resize(size_ + simdjson::SIMDJSON_PADDING, '\0');
    header_.payload_len = static_cast<std::uint32_t>(size_);
}

RecordView OwnedRecord::view() const {
    return RecordView{header_, PaddedView{bytes_.data(), size_, bytes_.size()}};
}

OwnedRecord market_record(StreamKind kind, std::uint16_t symbol, std::uint32_t epoch,
                          std::uint64_t conn_seq, std::int64_t recv_ns, const std::string& payload,
                          std::uint16_t shard) {
    RecordHeader h;
    h.kind = RecordKind::Market;
    h.stream = kind;
    h.symbol = symbol;
    h.shard = shard;
    h.epoch = epoch;
    h.conn_seq = conn_seq;
    h.recv_ns = recv_ns;
    return OwnedRecord(h, payload);
}

OwnedRecord snapshot_record(std::uint16_t symbol, std::uint32_t epoch, std::int64_t recv_ns,
                            const std::string& payload, std::uint16_t shard) {
    RecordHeader h;
    h.kind = RecordKind::Snapshot;
    h.symbol = symbol;
    h.shard = shard;
    h.epoch = epoch;
    h.recv_ns = recv_ns;
    return OwnedRecord(h, payload);
}

}  // namespace blc::test
