#include "blc/core/errors.hpp"
#include "blc/core/symbols.hpp"
#include "blc/net/rest_scheduler.hpp"
#include "blc/net/tls.hpp"

#include <functional>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

namespace blc {
namespace {

using namespace std::chrono_literals;

class FakeTransport final : public RestTransport {
public:
    struct Request {
        Endpoint endpoint;
        std::string target;
        std::function<void(HttpResult)> done;
    };
    void get(const Endpoint& endpoint, std::string target,
             std::function<void(HttpResult)> done) override {
        requests.push_back({endpoint, std::move(target), std::move(done)});
    }
    void cancel_all() noexcept override { ++cancels; }

    std::vector<Request> requests;
    int cancels = 0;
};

HttpResult ok_body(std::int64_t recv_ns = 111, std::int64_t steady_ns = 222) {
    HttpResult r;
    r.status = 200;
    r.body = R"({"lastUpdateId":1,"bids":[],"asks":[]})";
    r.recv_ns = recv_ns;
    r.steady_ns = steady_ns;
    return r;
}

struct Rig {
    Rig() {
        tuning.ring_retry = 0ms;
        tuning.backoff_initial = 0ms;
        scheduler = std::make_unique<RestScheduler>(io, Venue::Spot, default_endpoints(Venue::Spot),
                                                    tuning, transport, metrics);
    }
    void validate_ok(const std::string& list) {
        symbols = parse_symbol_list(Venue::Spot, list);
        scheduler->validate_symbols(symbols, [this](ValidationResult r) { results.push_back(r); });
        io.poll();
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            HttpResult r;
            r.status = 200;
            r.body = R"({"symbol":"X","price":"1.0"})";
            transport.requests[i].done(r);
        }
        io.poll();
        transport.requests.clear();
    }

    boost::asio::io_context io;
    NetTuning tuning;
    FakeTransport transport;
    IoMetrics metrics;
    std::unique_ptr<RestScheduler> scheduler;
    std::vector<SymbolSpec> symbols;
    std::vector<ValidationResult> results;
};

TEST(RestScheduler, ValidationOkThenSnapshotDelivered) {
    Rig rig;
    rig.validate_ok("BTCUSDT,ETHUSDT");
    ASSERT_EQ(rig.results.size(), 1u);
    EXPECT_EQ(rig.results[0].kind, ValidationResult::Kind::Ok);

    std::vector<std::pair<std::uint16_t, std::uint32_t>> delivered;
    rig.scheduler->set_delivery([&](std::uint16_t s, std::uint32_t epoch, const HttpResult&) {
        delivered.emplace_back(s, epoch);
        return DeliveryResult::Delivered;
    });
    rig.scheduler->want(1, (std::uint64_t{3} << 32) | 1);
    rig.io.poll();
    ASSERT_EQ(rig.transport.requests.size(), 1u);
    EXPECT_EQ(rig.transport.requests[0].endpoint.host, "api.binance.com");
    EXPECT_EQ(rig.transport.requests[0].target, "/api/v3/depth?symbol=ETHUSDT&limit=1000");
    rig.transport.requests[0].done(ok_body());
    rig.io.poll();
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(delivered[0].first, 1);
    EXPECT_EQ(delivered[0].second, 3u);
    EXPECT_EQ(rig.metrics.rest_requests.load(), 3u);  // 2 validations + 1 snapshot
    EXPECT_EQ(rig.metrics.rest_weight_used.load(), 2u * 2 + 50);
}

TEST(RestScheduler, RingFullRetryPreservesTimestamps) {
    Rig rig;
    rig.validate_ok("BTCUSDT");
    std::vector<std::pair<std::int64_t, std::int64_t>> seen;
    int calls = 0;
    rig.scheduler->set_delivery([&](std::uint16_t, std::uint32_t, const HttpResult& r) {
        seen.emplace_back(r.recv_ns, r.steady_ns);
        return ++calls < 3 ? DeliveryResult::RingFull : DeliveryResult::Delivered;
    });
    rig.scheduler->want(0, 1);
    rig.io.poll();
    ASSERT_EQ(rig.transport.requests.size(), 1u);
    rig.transport.requests[0].done(ok_body(5555, 7777));
    for (int i = 0; i < 20 && calls < 3; ++i) {
        rig.io.run_for(1ms);
    }
    ASSERT_EQ(seen.size(), 3u);
    for (const auto& s : seen) {
        EXPECT_EQ(s.first, 5555);
        EXPECT_EQ(s.second, 7777);
    }
}

TEST(RestScheduler, DiscardCounted) {
    Rig rig;
    rig.validate_ok("BTCUSDT");
    rig.scheduler->set_delivery(
        [](std::uint16_t, std::uint32_t, const HttpResult&) { return DeliveryResult::Discard; });
    rig.scheduler->want(0, 1);
    rig.io.poll();
    rig.transport.requests[0].done(ok_body());
    rig.io.poll();
    EXPECT_EQ(rig.metrics.snapshots_discarded.load(), 1u);
}

TEST(RestScheduler, StopCancelsAndIgnoresLateCallbacks) {
    Rig rig;
    rig.validate_ok("BTCUSDT,ETHUSDT");
    int deliveries = 0;
    rig.scheduler->set_delivery([&](std::uint16_t, std::uint32_t, const HttpResult&) {
        ++deliveries;
        return DeliveryResult::RingFull;
    });
    rig.scheduler->want(0, 1);
    rig.scheduler->want(1, 1);
    rig.io.poll();
    ASSERT_EQ(rig.transport.requests.size(), 2u);
    rig.transport.requests[0].done(ok_body());  // waits for ring space
    rig.io.poll();
    EXPECT_EQ(deliveries, 1);
    rig.scheduler->stop();
    rig.scheduler->stop();  // idempotent
    EXPECT_GE(rig.transport.cancels, 1);
    rig.transport.requests[1].done(ok_body());  // late completion after stop
    rig.scheduler->want(0, 2);
    rig.io.run_for(20ms);
    EXPECT_EQ(deliveries, 1);                      // no retry of the waiting body, no new delivery
    EXPECT_EQ(rig.transport.requests.size(), 2u);  // nothing dispatched after stop
}

TEST(RestScheduler, FatalSnapshotResponseReported) {
    Rig rig;
    rig.validate_ok("BTCUSDT");
    std::vector<std::string> fatal;
    rig.scheduler->set_fatal([&](std::string_view m) { fatal.emplace_back(m); });
    rig.scheduler->want(0, 1);
    rig.io.poll();
    HttpResult r;
    r.status = 400;
    r.body = R"({"code":-1121,"msg":"Invalid symbol."})";
    r.binance_error_code = -1121;
    rig.transport.requests[0].done(r);
    rig.io.poll();
    ASSERT_EQ(fatal.size(), 1u);
    EXPECT_NE(fatal[0].find("Invalid symbol"), std::string::npos) << fatal[0];
}

TEST(RestScheduler, ValidationInvalidSymbolNamesIt) {
    Rig rig;
    rig.symbols = parse_symbol_list(Venue::Spot, "BTCUSDT,BTCUSDX");
    rig.scheduler->validate_symbols(rig.symbols,
                                    [&](ValidationResult r) { rig.results.push_back(r); });
    rig.io.poll();
    ASSERT_EQ(rig.transport.requests.size(), 2u);
    EXPECT_EQ(rig.transport.requests[1].target, "/api/v3/ticker/price?symbol=BTCUSDX");
    HttpResult ok;
    ok.status = 200;
    ok.body = "{}";
    rig.transport.requests[0].done(ok);
    HttpResult bad;
    bad.status = 400;
    bad.body = R"({"code":-1121,"msg":"Invalid symbol."})";
    bad.binance_error_code = -1121;
    rig.transport.requests[1].done(bad);
    rig.io.poll();
    ASSERT_EQ(rig.results.size(), 1u);
    EXPECT_EQ(rig.results[0].kind, ValidationResult::Kind::InvalidSymbol);
    EXPECT_EQ(rig.results[0].symbol, "BTCUSDX");
}

TEST(RestScheduler, ValidationUnreachable) {
    Rig rig;
    rig.tuning.validation_give_up = 0ms;
    rig.scheduler =
        std::make_unique<RestScheduler>(rig.io, Venue::Spot, default_endpoints(Venue::Spot),
                                        rig.tuning, rig.transport, rig.metrics);
    rig.symbols = parse_symbol_list(Venue::Spot, "BTCUSDT");
    rig.scheduler->validate_symbols(rig.symbols,
                                    [&](ValidationResult r) { rig.results.push_back(r); });
    rig.io.poll();
    ASSERT_EQ(rig.transport.requests.size(), 1u);
    HttpResult r;
    r.transport_error = true;
    rig.transport.requests[0].done(r);
    rig.io.poll();
    ASSERT_EQ(rig.results.size(), 1u);
    EXPECT_EQ(rig.results[0].kind, ValidationResult::Kind::Unreachable);
}

TEST(Tls, ClientContext) {
    EXPECT_NO_THROW(make_tls_client_context(TlsConfig{}));
    TlsConfig bad;
    bad.extra_ca_pem = "-----BEGIN CERTIFICATE-----
        not a certificate-- -- -END CERTIFICATE-- -- -";
        EXPECT_THROW(make_tls_client_context(bad), IoError);
}

}  // namespace
}  // namespace blc
