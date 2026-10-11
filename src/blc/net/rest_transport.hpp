#pragma once

#include "blc/net/endpoints.hpp"
#include "blc/net/rest_scheduler.hpp"
#include "blc/parse/json_workspace.hpp"

#include <memory>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

namespace blc {

// HTTPS GET over Beast (design §6.3): TLS with SNI and hostname verification, one connection
// per request, rest_timeout for the whole exchange, body <= 1 MiB. Every 200 body is validated
// as JSON in the shared workspace before delivery (invalid JSON is a transport failure); a 400
// body's root "code" is extracted through the same workspace. Both clocks are captured when the
// response is complete.
class BeastRestTransport final : public RestTransport {
public:
    BeastRestTransport(boost::asio::io_context& io, boost::asio::ssl::context& tls,
                       JsonWorkspace& workspace, const NetTuning& tuning);
    ~BeastRestTransport() override;
    BeastRestTransport(const BeastRestTransport&) = delete;
    BeastRestTransport& operator=(const BeastRestTransport&) = delete;
    BeastRestTransport(BeastRestTransport&&) = delete;
    BeastRestTransport& operator=(BeastRestTransport&&) = delete;

    void get(const Endpoint& endpoint, std::string target,
             std::function<void(HttpResult)> done) override;
    void cancel_all() noexcept override;

    class Session;

private:
    boost::asio::io_context& io_;
    boost::asio::ssl::context& tls_;
    JsonWorkspace& workspace_;
    NetTuning tuning_;
    std::vector<std::weak_ptr<Session>> sessions_;
    bool stopped_ = false;
};

}  // namespace blc
