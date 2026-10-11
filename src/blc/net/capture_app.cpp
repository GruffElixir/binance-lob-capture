#include "blc/net/capture_app.hpp"

#include "blc/core/errors.hpp"
#include "blc/core/time.hpp"
#include "blc/net/rest_scheduler.hpp"
#include "blc/net/rest_transport.hpp"
#include "blc/net/ws_shard.hpp"
#include "blc/pipeline/consumer.hpp"
#include "blc/util/logging.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <fstream>
#include <thread>

#include <boost/asio/signal_set.hpp>
namespace blc {
namespace asio = boost::asio;
namespace {
std::size_t peak_rss_kib() {
#ifdef __linux__
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0) {
            return static_cast<std::size_t>(std::stoull(line.substr(6)));
        }
    }
#endif
    return 0;
}

class CaptureRun {
public:
    CaptureRun(const CaptureConfig& config, std::chrono::steady_clock::time_point start)
        : config_(config),
          start_(start),
          started_wall_ns_(config.started_wall_ns != 0 ? config.started_wall_ns : wall_clock_ns()),
          tls_(make_tls_client_context(config.tls)),
          signals_(io_, SIGINT, SIGTERM),
          duration_(io_),
          poll_(io_),
          deadline_(io_),
          ring_(SpscRing::capacity_for_symbols(config.options.symbols.size())),
          slots_(config.options.symbols.size()),
          transport_(io_, tls_, splitter_.json_workspace(), config.tuning),
          scheduler_(io_, config.options.venue, config.endpoints, config.tuning, transport_,
                     metrics_),
          limiter_(config.tuning.limiter_attempts, config.tuning.limiter_window),
          sidecar_(SymbolFiles::kBufferBytes) {
        const auto groups =
            make_shards(config.options.symbols.size(), config.options.symbols_per_connection);
        std::uint16_t first = 0;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            std::vector<SymbolSpec> symbols;
            for (const auto index : groups[i]) {
                symbols.push_back(config.options.symbols[index]);
                symbol_shards_.push_back(i);
            }
            auto table = std::make_unique<StreamTable>(symbols, first);
            auto target = ws_target(config.endpoints, *table);
            auto router = std::make_unique<FrameRouter>(static_cast<std::uint16_t>(i), *table,
                                                        splitter_, ring_, metrics_);
            auto shard = std::make_shared<WsShard>(
                io_, tls_, config.endpoints.ws, std::move(target), *router, limiter_, config.tuning,
                metrics_, std::chrono::seconds(config.options.fault_reconnect_every_s));
            if (config.buffer_capacity_observer) {
                shard->set_buffer_observer([this, i](std::size_t cap, bool after) {
                    config_.buffer_capacity_observer(static_cast<std::uint16_t>(i), cap, after);
                });
            }
            tables_.push_back(std::move(table));
            routers_.push_back(std::move(router));
            shards_.push_back(std::move(shard));
            first = static_cast<std::uint16_t>(first + symbols.size());
        }
        scheduler_.set_delivery(
            [this](std::uint16_t symbol, std::uint32_t epoch, const HttpResult& r) {
                auto& router = *routers_[symbol_shards_[symbol]];
                if (stopping_ || !router.can_deliver_snapshot(epoch))
                    return DeliveryResult::Discard;
                return router.push_snapshot(symbol, epoch, r.body, r.recv_ns, r.steady_ns)
                           ? DeliveryResult::Delivered
                           : DeliveryResult::RingFull;
            });
        scheduler_.set_fatal([this](std::string_view message) {
            log(LogLevel::Error, message);
            fatal_.store(1, std::memory_order_release);
            shutdown();
        });
    }
    CaptureRun(const CaptureRun&) = delete;
    CaptureRun& operator=(const CaptureRun&) = delete;
    CaptureRun(CaptureRun&&) = delete;
    CaptureRun& operator=(CaptureRun&&) = delete;
    ~CaptureRun() {
        scheduler_.stop();
        for (auto& shard : shards_)
            shard->finalize();
        ring_.close_producer();
        if (consumer_.joinable())
            consumer_.join();
    }
    int run() {
        arm_signals();
        if (config_.options.duration_s > 0) {
            duration_.expires_at(start_ + std::chrono::seconds(config_.options.duration_s));
            duration_.async_wait([this](const boost::system::error_code& ec) {
                if (!ec)
                    shutdown();
            });
        }
        scheduler_.validate_symbols(
            config_.options.symbols, [this](const ValidationResult& result) {
                if (stopping_)
                    return;
                if (result.kind != ValidationResult::Kind::Ok) {
                    log(LogLevel::Error, result.message);
                    fatal_.store(result.kind == ValidationResult::Kind::InvalidSymbol ? 2 : 1,
                                 std::memory_order_release);
                    shutdown();
                    return;
                }
                start_capture();
            });
        try {
            io_.run();
        } catch (...) {
            fatal_.store(1, std::memory_order_release);
            shutdown();
            io_.stop();
            ring_.close_producer();
            if (consumer_.joinable())
                consumer_.join();
            throw;
        }
        for (auto& shard : shards_)
            shard->finalize();
        ring_.close_producer();
        if (consumer_.joinable())
            consumer_.join();
        if (processor_) {
            const auto md = processor_->stats().md_rows;
            const auto frames = metrics_.frames_received.load(std::memory_order_relaxed);
            const auto malformed = metrics_.malformed_frames.load(std::memory_order_relaxed);
            const auto drops = metrics_.ring_drops.load(std::memory_order_relaxed);
            if (frames != md + malformed + drops && fatal_.load(std::memory_order_acquire) == 0) {
                log(LogLevel::Error, "frame accounting invariant failed");
                fatal_.store(1, std::memory_order_release);
            }
            log(LogLevel::Info,
                "totals frames=" + std::to_string(frames) + " md_rows=" + std::to_string(md) +
                    " malformed=" + std::to_string(malformed) + " drops=" + std::to_string(drops) +
                    " VmHWM=" + std::to_string(peak_rss_kib()) + " budget_mib=" +
                    std::to_string(budget_mib(config_.options.symbols.size(), shards_.size())));
        }
        return fatal_.load(std::memory_order_acquire);
    }

private:
    void start_capture() {
        const auto dir = prepare_output_dir(config_.options.output_dir);
        const auto now = wall_clock_ns();
        const auto date = utc_date(started_wall_ns_);
        std::vector<FileNames> names;
        std::vector<std::string> all_names;
        for (const auto& symbol : config_.options.symbols) {
            auto n = file_names(config_.options.venue, symbol.upper, date);
            all_names.insert(all_names.end(), {n.market_data, n.orderbook, n.snapshots});
            names.push_back(std::move(n));
        }
        archive_existing(dir, all_names, utc_compact_stamp(now));
        std::vector<std::unique_ptr<SymbolOutput>> outputs;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto& symbol = config_.options.symbols[i];
            if (config_.output_factory) {
                outputs.push_back(config_.output_factory(symbol));
            } else {
                auto files =
                    std::make_unique<SymbolFiles>(dir, names[i], sidecar_.data(), sidecar_.size());
                outputs.push_back(std::make_unique<FileOutput>(
                    std::move(files), config_.options.venue, symbol.upper));
            }
        }
        processor_ = std::make_unique<Processor>(
            ProcessorConfig{config_.options.venue, config_.options.symbols,
                            config_.options.trade_rows},
            std::move(outputs), slots_);
        ConsumerConfig consumer_config;
        consumer_config.steady_now_ns = steady_clock_ns;
        consumer_config.started_at_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(start_.time_since_epoch()).count();
        consumer_config.io = &metrics_;
        consumer_config.stats_every = std::chrono::seconds(config_.options.stats_interval_s);
        consumer_config.on_stats = [](const std::string& line) { log(LogLevel::Info, line); };
        consumer_ = std::thread(
            [this, consumer_config] { run_consumer(ring_, *processor_, fatal_, consumer_config); });
        for (auto& shard : shards_)
            shard->start();
        poll();
    }
    void arm_signals() {
        signals_.async_wait([this](const boost::system::error_code& ec, int) {
            if (ec)
                return;
            if (stopping_) {
                io_.stop();
                return;
            }
            arm_signals();
            shutdown();
        });
    }
    void poll() {
        poll_.expires_after(config_.tuning.poll_interval);
        poll_.async_wait([this](const boost::system::error_code& ec) {
            if (ec || stopping_)
                return;
            if (fatal_.load(std::memory_order_acquire) != 0) {
                shutdown();
                return;
            }
            slots_.scan([this](std::size_t symbol, std::uint32_t epoch, std::uint32_t number) {
                const auto value = (static_cast<std::uint64_t>(epoch) << 32) | number;
                scheduler_.want(static_cast<std::uint16_t>(symbol), value);
            });
            poll();
        });
    }
    void shutdown() {
        if (stopping_)
            return;
        stopping_ = true;
        duration_.cancel();
        poll_.cancel();
        scheduler_.stop();
        for (auto& shard : shards_)
            shard->shutdown();
        deadline_.expires_after(config_.tuning.shutdown_deadline);
        deadline_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec)
                io_.stop();
        });
        check_closed();
    }
    void check_closed() {
        const bool closed = std::all_of(shards_.begin(), shards_.end(), [](const auto& shard) {
            return shard->state() == WsShard::State::Closed;
        });
        if (closed) {
            deadline_.cancel();
            signals_.cancel();
            return;
        }
        poll_.expires_after(config_.tuning.poll_interval);
        poll_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec)
                check_closed();
        });
    }
    const CaptureConfig& config_;
    std::chrono::steady_clock::time_point start_;
    std::int64_t started_wall_ns_;
    EnvelopeSplitter splitter_;
    asio::ssl::context tls_;
    asio::io_context io_;
    asio::signal_set signals_;
    asio::steady_timer duration_, poll_, deadline_;
    SpscRing ring_;
    IoMetrics metrics_;
    RequestSlots slots_;
    BeastRestTransport transport_;
    RestScheduler scheduler_;
    AttemptLimiter limiter_;
    std::vector<char> sidecar_;
    std::vector<std::unique_ptr<StreamTable>> tables_;
    std::vector<std::unique_ptr<FrameRouter>> routers_;
    std::vector<std::shared_ptr<WsShard>> shards_;
    std::vector<std::size_t> symbol_shards_;
    std::unique_ptr<Processor> processor_;
    std::atomic<int> fatal_{0};
    std::thread consumer_;
    bool stopping_ = false;
};
}  // namespace

std::size_t budget_mib(std::size_t symbols, std::size_t connections) {
    constexpr std::size_t mib = std::size_t{1024} * 1024;
    const auto bytes = SpscRing::capacity_for_symbols(symbols) + 5 * mib + 6 * mib + 672000 +
                       65536 + mib + 8 * symbols + 883136 * symbols + 327680 * connections +
                       mib * connections + std::size_t{4} * 1114112 + 48 * mib;
    return (bytes + mib - 1) / mib;
}
int run_capture(const CaptureConfig& config, std::chrono::steady_clock::time_point started_at) {
    if (config.options.duration_s > 0 &&
        std::chrono::steady_clock::now() >=
            started_at + std::chrono::seconds(config.options.duration_s))
        return 0;
    if (config.options.symbols.empty())
        throw UsageError("capture requires symbols");
    CaptureRun capture(config, started_at);
    return capture.run();
}
}  // namespace blc
