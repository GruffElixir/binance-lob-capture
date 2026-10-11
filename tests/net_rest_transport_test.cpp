#include "blc/net/rest_transport.hpp"
#include "blc/net/tls.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace blc {
namespace {
TEST(RestTransport, DeadlineCoversPendingResolutionAndIgnoresLateCompletion) {
    boost::asio::io_context io;
    auto tls = make_tls_client_context({});
    JsonWorkspace workspace;
    NetTuning tuning;
    tuning.rest_timeout = std::chrono::milliseconds(20);
    BeastRestTransport::ResolveDone held;
    BeastRestTransport transport(
        io, tls, workspace, tuning,
        [&](const Endpoint&, BeastRestTransport::ResolveDone done) { held = std::move(done); });
    int completed = 0;
    transport.get({"127.0.0.1", "443", ""}, "/", [&](HttpResult result) {
        EXPECT_TRUE(result.transport_error);
        ++completed;
    });
    io.run_for(std::chrono::seconds(2));
    ASSERT_TRUE(held);
    EXPECT_EQ(completed, 1);
    held({}, {});
    io.restart();
    io.poll();
    EXPECT_EQ(completed, 1);
}
TEST(RestTransport, CancellationDuringResolutionDisablesDeadlineAndLateCallback) {
    boost::asio::io_context io;
    auto tls = make_tls_client_context({});
    JsonWorkspace workspace;
    NetTuning tuning;
    tuning.rest_timeout = std::chrono::milliseconds(20);
    BeastRestTransport::ResolveDone held;
    BeastRestTransport transport(
        io, tls, workspace, tuning,
        [&](const Endpoint&, BeastRestTransport::ResolveDone done) { held = std::move(done); });
    int completed = 0;
    transport.get({"127.0.0.1", "443", ""}, "/", [&](HttpResult) { ++completed; });
    transport.cancel_all();
    io.run_for(std::chrono::seconds(2));
    ASSERT_TRUE(held);
    held({}, {});
    io.restart();
    io.poll();
    EXPECT_EQ(completed, 0);
}
}  // namespace
}  // namespace blc
