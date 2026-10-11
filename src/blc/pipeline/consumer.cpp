#include "blc/pipeline/consumer.hpp"

#include "blc/core/time.hpp"
#include "blc/util/logging.hpp"

#include <exception>

namespace blc {

namespace {

constexpr auto kParkTimeout = std::chrono::milliseconds(50);

class Loop {
public:
    Loop(SpscRing& ring, Processor& processor, std::atomic<int>& fatal_code,
         const ConsumerConfig& config)
        : ring_(ring),
          processor_(processor),
          fatal_code_(fatal_code),
          config_(config),
          flush_ns_(std::chrono::nanoseconds(config.flush_every).count()),
          stats_ns_(std::chrono::nanoseconds(config.stats_every).count()),
          next_flush_(config.started_at_ns + flush_ns_),
          next_stats_(config.started_at_ns + stats_ns_) {}

    int run() noexcept {
        for (;;) {
            if (drain_some()) {
                periodic();
                continue;
            }
            periodic();
            if (ring_.producer_closed()) {
                // The producer's last pushes happen before it closes: one more pass sees them.
                while (drain_some()) {
                }
                break;
            }
            ring_.wait(kParkTimeout);
        }
        if (!processor_.finish()) {
            publish_fatal();
        }
        emit_stats("summary");
        return fatal_code_.load(std::memory_order_acquire);
    }

private:
    // Processes up to a batch of records; false if the ring was empty.
    bool drain_some() noexcept {
        RecordView v;
        int n = 0;
        while (n < 256 && ring_.try_peek(v)) {
            if (!processor_.fatal()) {
                processor_.process(v);
            }
            ring_.pop();
            ++n;
        }
        if (processor_.fatal()) {
            publish_fatal();
        }
        return n > 0;
    }

    void publish_fatal() noexcept {
        if (!published_) {
            published_ = true;
            fatal_code_.store(1, std::memory_order_release);
        }
    }

    std::int64_t now() const noexcept {
        return config_.steady_now_ns ? config_.steady_now_ns() : steady_clock_ns();
    }

    void periodic() noexcept {
        const std::int64_t t = now();
        if (flush_ns_ > 0 && t >= next_flush_) {
            processor_.flush();
            if (processor_.fatal()) {
                publish_fatal();
            }
            while (next_flush_ <= t) {
                next_flush_ += flush_ns_;
            }
        }
        if (stats_ns_ > 0 && t >= next_stats_) {
            emit_stats("stats");
            while (next_stats_ <= t) {
                next_stats_ += stats_ns_;
            }
        }
    }

    void emit_stats(const char* label) noexcept {
        if (!config_.on_stats) {
            return;
        }
        try {
            const std::int64_t uptime_s = (now() - config_.started_at_ns) / kNanosPerSecond;
            std::string line = format_stats(processor_.stats(), config_.io, ring_.capacity(),
                                            ring_.high_water(), uptime_s);
            line.replace(0, 5, label);  // "stats" -> label
            config_.on_stats(line);
        } catch (const std::exception& e) {
            log(LogLevel::Warn, e.what());
        }
    }

    SpscRing& ring_;
    Processor& processor_;
    std::atomic<int>& fatal_code_;
    const ConsumerConfig& config_;
    std::int64_t flush_ns_;
    std::int64_t stats_ns_;
    std::int64_t next_flush_;
    std::int64_t next_stats_;
    bool published_ = false;
};

}  // namespace

int run_consumer(SpscRing& ring, Processor& processor, std::atomic<int>& fatal_code,
                 const ConsumerConfig& config) noexcept {
    return Loop(ring, processor, fatal_code, config).run();
}

}  // namespace blc
