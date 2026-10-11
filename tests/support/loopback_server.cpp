#include "support/loopback_server.hpp"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace blc::test {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class LoopbackServer::Impl {
public:
    Impl(ServerScript s, const TestCa& ca)
        : script(std::move(s)),
          tls(asio::ssl::context::tls_server),
          ws_acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}),
          rest_acceptor(io, {asio::ip::make_address("127.0.0.1"), 0}) {
        tls.use_certificate_chain(asio::buffer(ca.certificate_pem));
        tls.use_private_key(asio::buffer(ca.key_pem), asio::ssl::context::pem);
        accept(true);
        accept(false);
        thread = std::thread([this] { io.run(); });
    }
    ~Impl() {
        io.stop();
        thread.join();
    }
    void event(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        ++events[name];
        if (name == "depth") {
            times.push_back(std::chrono::steady_clock::now());
        }
        cv.notify_all();
    }

    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(Impl& owner, tcp::socket socket, bool ws)
            : owner_(owner),
              stream_(std::move(socket), owner.tls),
              timer_(owner.io),
              ws_(ws),
              first_(ws && owner.ws_index++ == 0) {}
        void start() {
            if (ws_) {
                owner_.event("attempt");
                if (owner_.script.refuse_ws) {
                    beast::error_code ignored;
                    beast::get_lowest_layer(stream_).close(ignored);
                    return;
                }
                beast::get_lowest_layer(stream_).async_receive(
                    asio::buffer(hello_), tcp::socket::message_peek,
                    [self = shared_from_this()](beast::error_code ec, std::size_t) {
                        if (!ec) {
                            self->owner_.event("tls");
                            self->handshake();
                        }
                    });
            } else {
                handshake();
            }
        }

    private:
        void handshake() {
            later(ws_ ? owner_.script.tls_delay : std::chrono::milliseconds(0),
                  [self = shared_from_this()] {
                      self->stream_.next_layer().async_handshake(asio::ssl::stream_base::server,
                                                                 [self](beast::error_code ec) {
                                                                     if (!ec)
                                                                         self->read_request();
                                                                 });
                  });
        }

        template <class F>
        void later(std::chrono::milliseconds delay, F f) {
            timer_.expires_after(delay);
            timer_.async_wait([self = shared_from_this(), f = std::move(f)](beast::error_code ec) {
                if (!ec)
                    f();
            });
        }
        void read_request() {
            http::async_read(stream_.next_layer(), buffer_, request_,
                             [self = shared_from_this()](beast::error_code ec, std::size_t) {
                                 if (ec)
                                     return;
                                 if (self->ws_)
                                     self->upgrade();
                                 else
                                     self->reply();
                             });
        }
        void upgrade() {
            owner_.event("upgrade");
            later(owner_.script.ws_delay, [self = shared_from_this()] {
                self->stream_.async_accept(self->request_, [self](beast::error_code ec) {
                    if (!ec) {
                        self->owner_.event("open");
                        self->read_control();
                        self->send();
                    }
                });
            });
        }
        void read_control() {
            if (owner_.script.hold_close) {
                stream_.next_layer().async_read_some(
                    asio::buffer(hello_),
                    [self = shared_from_this()](beast::error_code ec, std::size_t) {
                        if (!ec)
                            self->owner_.event("close_requested");
                    });
                return;
            }
            stream_.async_read(control_,
                               [self = shared_from_this()](beast::error_code ec, std::size_t) {
                                   if (ec == websocket::error::closed)
                                       self->owner_.event("closed");
                                   if (!ec) {
                                       self->control_.consume(self->control_.size());
                                       self->read_control();
                                   }
                               });
        }
        void send() {
            const auto& frames = first_ && !owner_.script.first_connection_frames.empty()
                                     ? owner_.script.first_connection_frames
                                     : owner_.script.frames;
            if (next_ == frames.size()) {
                if (owner_.script.force_close &&
                    (!owner_.script.force_close_first_only || first_) &&
                    (!owner_.script.force_close_ready || owner_.script.force_close_ready())) {
                    beast::error_code ignored;
                    beast::get_lowest_layer(stream_).close(ignored);
                    return;
                }
                if (!owner_.script.repeat_frames || frames.empty())
                    return;
                next_ = 0;
            }
            stream_.text(true);
            if (owner_.script.fragment_bytes > 0) {
                stream_.auto_fragment(true);
                stream_.write_buffer_bytes(owner_.script.fragment_bytes);
            }
            stream_.async_write(asio::buffer(frames[next_++]),
                                [self = shared_from_this()](beast::error_code ec, std::size_t) {
                                    if (!ec) {
                                        self->owner_.event("frame");
                                        self->later(self->owner_.script.frame_interval,
                                                    [self] { self->send(); });
                                    }
                                });
        }
        void reply() {
            const std::string target(request_.target());
            TestReply reply;
            auto delay = owner_.script.ticker_delay;
            if (target.find("ticker") != std::string::npos) {
                owner_.event("ticker");
                reply.body = R"({"symbol":"BTCUSDT","price":"100"})";
                if (target.find("BTCUSDX") != std::string::npos) {
                    reply.status = 400;
                    reply.body = R"({"code":-1121,"msg":"Invalid symbol."})";
                }
            } else {
                delay = owner_.script.depth_delay;
                const auto index = owner_.depth_index++;
                owner_.event("depth");
                if (!owner_.script.depth_replies.empty()) {
                    reply =
                        owner_.script
                            .depth_replies[std::min(index, owner_.script.depth_replies.size() - 1)];
                } else {
                    reply.body =
                        R"({"lastUpdateId":100,"bids":[["100","2"]],"asks":[["101","3"]]})";
                }
            }
            response_.version(11);
            response_.result(reply.status);
            response_.set(http::field::content_type, "application/json");
            if (reply.retry_after_s > 0)
                response_.set(http::field::retry_after, std::to_string(reply.retry_after_s));
            response_.body() = std::move(reply.body);
            response_.prepare_payload();
            later(delay, [self = shared_from_this()] {
                http::async_write(self->stream_.next_layer(), self->response_,
                                  [self](beast::error_code, std::size_t) {
                                      beast::error_code ignored;
                                      beast::get_lowest_layer(self->stream_).close(ignored);
                                  });
            });
        }
        Impl& owner_;
        websocket::stream<asio::ssl::stream<tcp::socket>> stream_;
        asio::steady_timer timer_;
        beast::flat_buffer buffer_, control_;
        http::request<http::string_body> request_;
        http::response<http::string_body> response_;
        bool ws_;
        bool first_;
        std::array<char, 5> hello_{};
        std::size_t next_ = 0;
    };

    void accept(bool ws) {
        auto& acceptor = ws ? ws_acceptor : rest_acceptor;
        acceptor.async_accept([this, ws](beast::error_code ec, tcp::socket socket) {
            if (!ec)
                std::make_shared<Session>(*this, std::move(socket), ws)->start();
            if (ec != asio::error::operation_aborted)
                accept(ws);
        });
    }
    ServerScript script;
    asio::io_context io;
    asio::ssl::context tls;
    tcp::acceptor ws_acceptor, rest_acceptor;
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::size_t> events;
    std::vector<std::chrono::steady_clock::time_point> times;
    std::size_t depth_index = 0;
    std::size_t ws_index = 0;
};

LoopbackServer::LoopbackServer(ServerScript script, const TestCa& ca)
    : impl_(std::make_unique<Impl>(std::move(script), ca)) {}
LoopbackServer::~LoopbackServer() = default;
VenueEndpoints LoopbackServer::endpoints(Venue venue) const {
    auto endpoints = default_endpoints(venue);
    endpoints.ws.host = endpoints.rest.host = "127.0.0.1";
    endpoints.ws.port = std::to_string(impl_->ws_acceptor.local_endpoint().port());
    endpoints.rest.port = std::to_string(impl_->rest_acceptor.local_endpoint().port());
    return endpoints;
}
bool LoopbackServer::wait_for(std::string event, std::size_t count, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->cv.wait_for(lock, timeout, [&] { return impl_->events[event] >= count; });
}
std::size_t LoopbackServer::count(const std::string& event) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->events.find(event);
    return it == impl_->events.end() ? 0 : it->second;
}
std::vector<std::chrono::steady_clock::time_point> LoopbackServer::depth_times() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->times;
}
}  // namespace blc::test
