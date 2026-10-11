#pragma once

#include "blc/net/endpoints.hpp"
#include "support/test_ca.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace blc::test {
struct TestReply {
    unsigned status = 200;
    std::string body;
    int retry_after_s = 0;
};
struct ServerScript {
    std::vector<std::string> frames;
    std::vector<std::string> first_connection_frames;
    std::vector<TestReply> depth_replies;
    std::chrono::milliseconds frame_interval{30};
    std::chrono::milliseconds tls_delay{0};
    std::chrono::milliseconds ws_delay{0};
    std::chrono::milliseconds ticker_delay{0};
    std::chrono::milliseconds depth_delay{0};
    bool force_close = false;
    bool repeat_frames = true;
    bool refuse_ws = false;
    bool hold_close = false;
    std::size_t fragment_bytes = 0;
};
class LoopbackServer {
public:
    explicit LoopbackServer(ServerScript script, const TestCa& ca);
    ~LoopbackServer();
    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;
    VenueEndpoints endpoints(Venue venue) const;
    bool wait_for(std::string event, std::size_t count = 1,
                  std::chrono::seconds timeout = std::chrono::seconds(10));
    std::size_t count(const std::string& event) const;
    std::vector<std::chrono::steady_clock::time_point> depth_times() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace blc::test
