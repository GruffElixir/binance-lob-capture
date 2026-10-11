#include "blc/parse/payload_parser.hpp"
#include "support/fixtures.hpp"

#include <string>

#include <gtest/gtest.h>

namespace blc {
namespace {

using test::load_fixture;
using test::PaddedBuffer;

ParseStatus parse_diff(PayloadParser& p, Venue v, const std::string& json, DepthDiff& out) {
    PaddedBuffer b(json);
    return p.diff(v, b.view(), "BTCUSDT", out);
}

std::string levels_json(int n, bool zero_qty_ok = true) {
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        s += (i ? "," : "") + std::string("[\"") + std::to_string(100000 - i) + ".5\",\"" +
             (zero_qty_ok && i % 7 == 0 ? "0" : "1.25") + "\"]";
    }
    return s + "]";
}

std::string spot_diff_with(const std::string& bids, const std::string& asks) {
    return R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":10,"u":12,"b":)" + bids + R"(,"a":)" +
           asks + "}";
}

TEST(Payload, SpotDiffFixture) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    ASSERT_EQ(parse_diff(p, Venue::Spot, load_fixture("payloads/spot_diff.json"), d),
              ParseStatus::Ok);
    EXPECT_EQ(d.first_id, 101280538155u);
    EXPECT_EQ(d.last_id, 101280538165u);
    EXPECT_FALSE(d.has_prev);
    ASSERT_EQ(d.bids.size(), 6u);
    ASSERT_EQ(d.asks.size(), 2u);
    EXPECT_EQ(d.bids[0], (Level{8313464000000, 7000}));
    EXPECT_EQ(d.asks[0], (Level{8315010000000, 838136000}));
}

TEST(Payload, UsdmDiffHasPrev) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    ASSERT_EQ(parse_diff(p, Venue::Usdm, load_fixture("payloads/usdm_diff.json"), d),
              ParseStatus::Ok);
    EXPECT_TRUE(d.has_prev);
    EXPECT_EQ(d.first_id, 11788562486118u);
    EXPECT_EQ(d.last_id, 11788562491279u);
    EXPECT_EQ(d.prev_last_id, 11788562485868u);
    ASSERT_FALSE(d.bids.empty());
    EXPECT_EQ(d.bids[0], (Level{100000000000, 11286300000}));
}

TEST(Payload, SpotDepth5IdIsLastUpdateId) {
    PayloadParser p;
    Depth5 d{};
    PaddedBuffer b(load_fixture("payloads/spot_depth5.json"));
    ASSERT_EQ(p.depth5(Venue::Spot, b.view(), "BTCUSDT", d), ParseStatus::Ok);
    EXPECT_EQ(d.id, 101280538165u);
    EXPECT_EQ(d.n_bids, 5);
    EXPECT_EQ(d.n_asks, 5);
    EXPECT_EQ(d.bids[0], (Level{8315009000000, 227294000}));
    EXPECT_EQ(d.asks[4], (Level{8315015000000, 5000000}));
}

TEST(Payload, UsdmDepth5IdIsU) {
    PayloadParser p;
    Depth5 d{};
    PaddedBuffer b(load_fixture("payloads/usdm_depth5.json"));
    ASSERT_EQ(p.depth5(Venue::Usdm, b.view(), "BTCUSDT", d), ParseStatus::Ok);
    EXPECT_EQ(d.id, 11788554250933u);
    EXPECT_EQ(d.n_bids, 5);
    EXPECT_EQ(d.bids[0], (Level{8310160000000, 359200000}));
}

TEST(Payload, TradeMaker) {
    PayloadParser p;
    Trade t{};
    PaddedBuffer spot(load_fixture("payloads/spot_trade.json"));
    ASSERT_EQ(p.trade(Venue::Spot, spot.view(), "BTCUSDT", t), ParseStatus::Ok);
    EXPECT_TRUE(t.buyer_is_maker);
    PaddedBuffer usdm(load_fixture("payloads/usdm_trade.json"));
    ASSERT_EQ(p.trade(Venue::Usdm, usdm.view(), "BTCUSDT", t), ParseStatus::Ok);
    EXPECT_TRUE(t.buyer_is_maker);
    PaddedBuffer taker(R"({"e":"trade","s":"BTCUSDT","m":false,"p":"1","q":"1"})");
    ASSERT_EQ(p.trade(Venue::Spot, taker.view(), "BTCUSDT", t), ParseStatus::Ok);
    EXPECT_FALSE(t.buyer_is_maker);
}

TEST(Payload, Snapshots) {
    PayloadParser p;
    auto s = PayloadParser::make_snapshot();
    PaddedBuffer spot(load_fixture("payloads/spot_snapshot.json"));
    ASSERT_EQ(p.snapshot(Venue::Spot, spot.view(), s), ParseStatus::Ok);
    EXPECT_EQ(s.last_update_id, 101280538111u);
    EXPECT_EQ(s.bids.size(), 5u);
    PaddedBuffer usdm(load_fixture("payloads/usdm_snapshot.json"));
    ASSERT_EQ(p.snapshot(Venue::Usdm, usdm.view(), s), ParseStatus::Ok);
    EXPECT_EQ(s.last_update_id, 11788554199923u);
    EXPECT_EQ(s.asks[0], (Level{8310170000000, 1439100000}));
}

TEST(Payload, FieldOrderIndependent) {
    PayloadParser p;
    auto a = PayloadParser::make_diff();
    auto b = PayloadParser::make_diff();
    ASSERT_EQ(
        parse_diff(
            p, Venue::Spot,
            R"({"e":"depthUpdate","E":1,"s":"BTCUSDT","U":10,"u":12,"b":[["1.5","2"]],"a":[["2.5","3"]]})",
            a),
        ParseStatus::Ok);
    ASSERT_EQ(
        parse_diff(
            p, Venue::Spot,
            R"({"a":[["2.5","3"]],"u":12,"b":[["1.5","2"]],"U":10,"s":"BTCUSDT","E":1,"e":"depthUpdate"})",
            b),
        ParseStatus::Ok);
    EXPECT_EQ(a.first_id, b.first_id);
    EXPECT_EQ(a.last_id, b.last_id);
    EXPECT_EQ(a.bids, b.bids);
    EXPECT_EQ(a.asks, b.asks);
}

TEST(Payload, InvalidIgnoredValuesRejected) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    const std::string base_end = R"("s":"BTCUSDT","U":10,"u":12,"b":[],"a":[]})";
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"E":nul,)" + base_end, d), ParseStatus::Malformed);
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"x":"\q",)" + base_end, d), ParseStatus::Malformed);
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"extra":[1,],)" + base_end, d),
              ParseStatus::Malformed);
    EXPECT_EQ(parse_diff(p, Venue::Spot,
                         R"({"nested":{"a":[1,{"b":null,"c":true}],"big":1e400},)" + base_end, d),
              ParseStatus::Ok);
    std::string deep = R"({"deep":)";
    for (int i = 0; i < 200; ++i) {
        deep += "[";
    }
    for (int i = 0; i < 200; ++i) {
        deep += "]";
    }
    EXPECT_EQ(parse_diff(p, Venue::Spot, deep + "," + base_end, d), ParseStatus::Oversized);
}

TEST(Payload, InvalidLevelDomainsRejected) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(R"([["0","1"]])", "[]"), d),
              ParseStatus::Malformed);  // zero price
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(R"([["1","0"]])", "[]"), d),
              ParseStatus::Ok);  // diff deletion
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"s":"BTCUSDT","U":13,"u":12,"b":[],"a":[]})", d),
              ParseStatus::Malformed);  // U > u

    auto s = PayloadParser::make_snapshot();
    const auto snap = [&](const std::string& bids, const std::string& asks) {
        PaddedBuffer b(R"({"lastUpdateId":5,"bids":)" + bids + R"(,"asks":)" + asks + "}");
        return p.snapshot(Venue::Spot, b.view(), s);
    };
    EXPECT_EQ(snap(R"([["2","0"]])", "[]"), ParseStatus::Malformed);            // zero qty
    EXPECT_EQ(snap(R"([["2","1"],["2","1"]])", "[]"), ParseStatus::Malformed);  // duplicate
    EXPECT_EQ(snap(R"([["1","1"],["2","1"]])", "[]"), ParseStatus::Malformed);  // bids ascending
    EXPECT_EQ(snap("[]", R"([["2","1"],["1","1"]])"), ParseStatus::Malformed);  // asks descending
    EXPECT_EQ(snap(R"([["2","1"],["1","1"]])", R"([["3","1"]])"), ParseStatus::Ok);
    EXPECT_EQ(snap("[]", "[]"), ParseStatus::Ok);  // empty sides allowed

    Depth5 d5{};
    PaddedBuffer bad5(R"({"lastUpdateId":5,"bids":[["1","1"],["2","1"]],"asks":[]})");
    EXPECT_EQ(p.depth5(Venue::Spot, bad5.view(), "BTCUSDT", d5), ParseStatus::Malformed);
    PaddedBuffer zero5(R"({"lastUpdateId":5,"bids":[["1","0"]],"asks":[]})");
    EXPECT_EQ(p.depth5(Venue::Spot, zero5.view(), "BTCUSDT", d5), ParseStatus::Malformed);
}

TEST(Payload, Errors) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"s":"BTCUSDT","U":10,"b":[],"a":[]})", d),
              ParseStatus::Malformed);  // missing u
    EXPECT_EQ(
        parse_diff(p, Venue::Spot, R"({"s":"BTCUSDT","U":10,"U":10,"u":12,"b":[],"a":[]})", d),
        ParseStatus::Malformed);  // duplicate U
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(R"([["1e5","1"]])", "[]"), d),
              ParseStatus::BadDecimal);
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"s":"ETHUSDT","U":10,"u":12,"b":[],"a":[]})", d),
              ParseStatus::SymbolMismatch);
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(R"([["1"]])", "[]"), d),
              ParseStatus::Malformed);
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(R"([["1","1","1"]])", "[]"), d),
              ParseStatus::Malformed);
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with("[]", "[]") + "x", d),
              ParseStatus::Malformed);  // trailing content
    EXPECT_EQ(parse_diff(p, Venue::Usdm, spot_diff_with("[]", "[]"), d),
              ParseStatus::Malformed);  // usdm needs pu
    EXPECT_EQ(parse_diff(p, Venue::Spot, R"({"U":10,"u":12,"b":[],"a":[]})", d),
              ParseStatus::Malformed);  // missing s

    Depth5 d5{};
    PaddedBuffer six(
        R"({"lastUpdateId":5,"bids":[["6","1"],["5","1"],["4","1"],["3","1"],["2","1"],["1","1"]],"asks":[]})");
    EXPECT_EQ(p.depth5(Venue::Spot, six.view(), "BTCUSDT", d5), ParseStatus::Malformed);
    PaddedBuffer wrong(R"({"s":"ETHUSDT","u":5,"b":[],"a":[]})");
    EXPECT_EQ(p.depth5(Venue::Usdm, wrong.view(), "BTCUSDT", d5), ParseStatus::SymbolMismatch);

    Trade t{};
    PaddedBuffer no_m(R"({"s":"BTCUSDT"})");
    EXPECT_EQ(p.trade(Venue::Spot, no_m.view(), "BTCUSDT", t), ParseStatus::Malformed);
}

TEST(Payload, OversizedDiffRejectedNotTruncated) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    EXPECT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(levels_json(20'000), "[]"), d),
              ParseStatus::Ok);
    EXPECT_EQ(d.bids.size(), 20'000u);
    EXPECT_EQ(
        parse_diff(p, Venue::Spot, spot_diff_with(levels_json(10'000), levels_json(10'001)), d),
        ParseStatus::Oversized);
}

TEST(Payload, OversizedSnapshot) {
    PayloadParser p;
    auto s = PayloadParser::make_snapshot();
    PaddedBuffer b(R"({"lastUpdateId":5,"bids":)" + levels_json(1'001, false) + R"(,"asks":[]})");
    EXPECT_EQ(p.snapshot(Venue::Spot, b.view(), s), ParseStatus::Oversized);
}

TEST(Payload, ScratchNeverGrows) {
    PayloadParser p;
    auto d = PayloadParser::make_diff();
    const auto bid_cap = d.bids.capacity();
    const auto ask_cap = d.asks.capacity();
    EXPECT_GE(bid_cap, kMaxDiffLevels);
    EXPECT_GE(ask_cap, kMaxDiffLevels);
    ASSERT_EQ(parse_diff(p, Venue::Spot, spot_diff_with(levels_json(20'000), "[]"), d),
              ParseStatus::Ok);
    ASSERT_EQ(parse_diff(p, Venue::Spot, spot_diff_with("[]", levels_json(20'000)), d),
              ParseStatus::Ok);
    EXPECT_EQ(d.bids.capacity(), bid_cap);
    EXPECT_EQ(d.asks.capacity(), ask_cap);
}

}  // namespace
}  // namespace blc
