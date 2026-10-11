#pragma once

#include "blc/book/order_book.hpp"
#include "blc/core/types.hpp"
#include "blc/parse/events.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <vector>

namespace blc::test {

// A reference "exchange" for property tests: it owns the true full-depth book and produces the
// streams Binance would send (diffs with ids, depth5 at exact or intermediate ids, trades),
// truncated REST snapshots, lost diffs (gaps) and reconnects.
class ExchangeSim {
public:
    struct Options {
        double p_depth5 = 0.15;
        double p_trade = 0.15;
        double p_gap = 1.0 / 2000;
        double p_reconnect = 1.0 / 2000;
        bool drift_down = false;  // the mid falls every diff, depleting the bid side
        int initial_levels = 3000;
    };

    struct Step {
        enum Kind { Diff, Depth5, Trade, Reconnect } kind = Diff;
        std::uint32_t epoch = 0;
        DepthDiff diff;
        blc::Depth5 d5{};
        blc::Trade trade{};
    };

    ExchangeSim(Venue venue, std::uint32_t seed);
    ExchangeSim(Venue venue, std::uint32_t seed, Options options);

    Step next();
    TopOfBook true_top() const;
    // Top 1000 per side. spot: lastUpdateId = current id. usdm: taken mid-way through the next
    // diff's id range (U <= L <= u), at the true intermediate state, as Binance futures does.
    DepthSnapshot snapshot();
    void inject_gap() { gap_requested_ = true; }
    void inject_reconnect() { reconnect_requested_ = true; }

    // Independent restatement of §5.3: could a book built from `served` (plus every diff since)
    // still show the true top 5? Diff-created levels beyond the boundary never count.
    bool coverage_sufficient(const DepthSnapshot& served) const;

    std::uint64_t current_id() const { return last_u_; }
    std::uint32_t epoch() const { return epoch_; }
    std::uint64_t lost_diffs() const { return lost_diffs_; }

private:
    struct Update {
        bool bid;
        std::int64_t price;
        std::int64_t qty;
        std::uint64_t id;
    };
    struct Pending {
        DepthDiff diff;
        std::vector<Update> updates;
    };

    Pending generate();
    void apply(const Pending& p);
    void lose_one();
    blc::Depth5 depth5_at(std::uint64_t p, const Pending* upcoming) const;

    Venue venue_;
    Options options_;
    std::mt19937 rng_;
    std::map<std::int64_t, std::int64_t, std::greater<>> bids_;  // best first
    std::map<std::int64_t, std::int64_t> asks_;                  // best first
    std::int64_t mid_ = 1'000'000;
    std::uint64_t last_u_ = 1'000;
    std::uint32_t epoch_ = 0;
    std::optional<Pending> queued_;
    bool gap_requested_ = false;
    bool reconnect_requested_ = false;
    bool force_diff_ = false;
    std::uint64_t lost_diffs_ = 0;
};

}  // namespace blc::test
