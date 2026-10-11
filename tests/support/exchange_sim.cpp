#include "support/exchange_sim.hpp"

#include <algorithm>

namespace blc::test {
namespace {

template <class Map>
void top_from(const Map& side, const std::map<std::int64_t, std::int64_t>& overlay, bool bids,
              std::array<Level, 5>& out, std::uint8_t* n) {
    // Merge the side with the overlay (final quantities of updates up to p), best first.
    std::vector<Level> candidates;
    int taken = 0;
    for (const auto& [price, qty] : side) {
        if (taken++ >= 5 + static_cast<int>(overlay.size())) {
            break;
        }
        const auto it = overlay.find(price);
        const std::int64_t q = it == overlay.end() ? qty : it->second;
        if (q > 0) {
            candidates.push_back(Level{price, q});
        }
    }
    for (const auto& [price, qty] : overlay) {
        if (qty > 0 && side.find(price) == side.end()) {
            candidates.push_back(Level{price, qty});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [bids](const Level& a, const Level& b) {
        return bids ? a.price > b.price : a.price < b.price;
    });
    std::uint8_t count = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i < candidates.size()) {
            out[i] = candidates[i];
            ++count;
        } else {
            out[i] = Level{0, 0};
        }
    }
    if (n != nullptr) {
        *n = count;
    }
}

}  // namespace

ExchangeSim::ExchangeSim(Venue venue, std::uint32_t seed) : ExchangeSim(venue, seed, Options{}) {}

ExchangeSim::ExchangeSim(Venue venue, std::uint32_t seed, Options options)
    : venue_(venue), options_(options), rng_(seed) {
    for (int i = 1; i <= options_.initial_levels; ++i) {
        bids_[mid_ - i] = 1 + i % 7;
        asks_[mid_ + i] = 1 + i % 5;
    }
}

ExchangeSim::Pending ExchangeSim::generate() {
    Pending p;
    std::uniform_int_distribution<int> n_updates(1, 6);
    std::uniform_int_distribution<int> offset(1, 30);
    std::uniform_int_distribution<int> qty(0, 5);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    std::uint64_t U = last_u_ + 1;
    if (venue_ == Venue::Usdm) {
        U += static_cast<std::uint64_t>(std::uniform_int_distribution<int>(0, 3)(rng_));
    }
    std::uint64_t id = U;
    std::int64_t mid = mid_;

    const auto add = [&](bool bid, std::int64_t price, std::int64_t q) {
        p.updates.push_back(Update{bid, price, q, id});
        if (venue_ == Venue::Usdm) {
            id += 1 + static_cast<std::uint64_t>(std::uniform_int_distribution<int>(0, 1)(rng_));
        } else {
            ++id;
        }
    };

    if (options_.drift_down) {
        // Move the mid down 20 ticks: every bid at or above the new mid is removed.
        const std::int64_t new_mid = mid - 20;
        for (auto it = bids_.begin(); it != bids_.end() && it->first >= new_mid; ++it) {
            add(true, it->first, 0);
        }
        mid = new_mid;
        for (int i = 1; i <= 3; ++i) {
            add(false, mid + i, 1 + i);
        }
    } else if (uni(rng_) < 0.05) {
        // Drift the mid by one tick, deleting the level that would cross.
        if (coin(rng_) == 0) {
            add(false, mid + 1, 0);
            ++mid;
        } else {
            add(true, mid - 1, 0);
            --mid;
        }
    }
    const int k = n_updates(rng_);
    for (int i = 0; i < k; ++i) {
        const bool bid = coin(rng_) == 0;
        const std::int64_t price = bid ? mid - offset(rng_) : mid + offset(rng_);
        add(bid, price, qty(rng_));
    }
    mid_ = mid;
    const std::uint64_t u = id - 1;

    // The diff carries each touched price once, with its final quantity.
    std::map<std::int64_t, std::int64_t, std::greater<>> bid_final;
    std::map<std::int64_t, std::int64_t> ask_final;
    for (const auto& up : p.updates) {
        (up.bid ? bid_final[up.price] : ask_final[up.price]) = up.qty;
    }
    p.diff.first_id = U;
    p.diff.last_id = u;
    p.diff.prev_last_id = last_u_;
    p.diff.has_prev = venue_ == Venue::Usdm;
    for (const auto& [price, q] : bid_final) {
        p.diff.bids.push_back(Level{price, q});
    }
    for (const auto& [price, q] : ask_final) {
        p.diff.asks.push_back(Level{price, q});
    }
    return p;
}

void ExchangeSim::apply(const Pending& p) {
    for (const auto& up : p.updates) {
        if (up.bid) {
            if (up.qty == 0) {
                bids_.erase(up.price);
            } else {
                bids_[up.price] = up.qty;
            }
        } else {
            if (up.qty == 0) {
                asks_.erase(up.price);
            } else {
                asks_[up.price] = up.qty;
            }
        }
    }
    last_u_ = p.diff.last_id;
}

void ExchangeSim::lose_one() {
    Pending lost = queued_ ? std::move(*queued_) : generate();
    queued_.reset();
    apply(lost);
    ++lost_diffs_;
}

blc::Depth5 ExchangeSim::depth5_at(std::uint64_t p, const Pending* upcoming) const {
    std::map<std::int64_t, std::int64_t> bid_overlay;
    std::map<std::int64_t, std::int64_t> ask_overlay;
    if (upcoming != nullptr) {
        for (const auto& up : upcoming->updates) {
            if (up.id <= p) {
                (up.bid ? bid_overlay[up.price] : ask_overlay[up.price]) = up.qty;
            }
        }
    }
    blc::Depth5 d{};
    d.id = p;
    top_from(bids_, bid_overlay, true, d.bids, &d.n_bids);
    top_from(asks_, ask_overlay, false, d.asks, &d.n_asks);
    return d;
}

ExchangeSim::Step ExchangeSim::next() {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    if (!force_diff_ && !queued_) {
        if (uni(rng_) < options_.p_gap) {
            gap_requested_ = true;
        }
        if (uni(rng_) < options_.p_reconnect) {
            reconnect_requested_ = true;
        }
    }
    Step step;
    if (reconnect_requested_) {
        reconnect_requested_ = false;
        ++epoch_;
        // Downtime: a few diffs happen while disconnected and are never delivered.
        const int lost = std::uniform_int_distribution<int>(0, 3)(rng_);
        for (int i = 0; i < lost; ++i) {
            lose_one();
        }
        step.kind = Step::Reconnect;
        step.epoch = epoch_;
        return step;
    }
    step.epoch = epoch_;
    if (gap_requested_ && !force_diff_) {
        gap_requested_ = false;
        lose_one();
        force_diff_ = true;  // the very next record is the diff that reveals the gap
    }

    const double r = uni(rng_);
    if (!force_diff_ && !queued_ && r < options_.p_depth5) {
        step.kind = Step::Depth5;
        if (venue_ == Venue::Spot && std::uniform_int_distribution<int>(0, 1)(rng_) == 0) {
            step.d5 = depth5_at(last_u_, nullptr);  // p == id
        } else {
            queued_ = generate();
            const std::uint64_t lo = last_u_ + 1;
            const std::uint64_t hi = queued_->diff.last_id;
            std::uint64_t p = hi;
            if (venue_ == Venue::Usdm) {
                p = std::uniform_int_distribution<std::uint64_t>(lo, hi)(rng_);
            }
            step.d5 = depth5_at(p, &*queued_);
        }
        return step;
    }
    if (!force_diff_ && r < options_.p_depth5 + options_.p_trade) {
        step.kind = Step::Trade;
        step.trade.buyer_is_maker = std::uniform_int_distribution<int>(0, 1)(rng_) == 0;
        return step;
    }
    force_diff_ = false;
    Pending p = queued_ ? std::move(*queued_) : generate();
    queued_.reset();
    apply(p);
    step.kind = Step::Diff;
    step.diff = std::move(p.diff);
    return step;
}

TopOfBook ExchangeSim::true_top() const {
    TopOfBook t;
    const std::map<std::int64_t, std::int64_t> none;
    top_from(bids_, none, true, t.bids, nullptr);
    top_from(asks_, none, false, t.asks, nullptr);
    return t;
}

DepthSnapshot ExchangeSim::snapshot() {
    DepthSnapshot s;
    s.last_update_id = last_u_;
    std::map<std::int64_t, std::int64_t> bid_overlay;
    std::map<std::int64_t, std::int64_t> ask_overlay;
    if (venue_ == Venue::Usdm) {
        if (!queued_) {
            queued_ = generate();
        }
        const std::uint64_t L = std::uniform_int_distribution<std::uint64_t>(
            queued_->diff.first_id, queued_->diff.last_id)(rng_);
        s.last_update_id = L;
        for (const auto& up : queued_->updates) {
            if (up.id <= L) {
                (up.bid ? bid_overlay[up.price] : ask_overlay[up.price]) = up.qty;
            }
        }
    }
    const auto fill = [](const auto& side, const std::map<std::int64_t, std::int64_t>& overlay,
                         bool bids, std::vector<Level>& out) {
        std::vector<Level> merged;
        std::size_t scanned = 0;
        for (const auto& [price, qty] : side) {
            if (scanned++ >= kSnapshotLimit + overlay.size()) {
                break;
            }
            const auto it = overlay.find(price);
            const std::int64_t q = it == overlay.end() ? qty : it->second;
            if (q > 0) {
                merged.push_back(Level{price, q});
            }
        }
        for (const auto& [price, qty] : overlay) {
            if (qty > 0 && side.find(price) == side.end()) {
                merged.push_back(Level{price, qty});
            }
        }
        std::sort(merged.begin(), merged.end(), [bids](const Level& a, const Level& b) {
            return bids ? a.price > b.price : a.price < b.price;
        });
        if (merged.size() > kSnapshotLimit) {
            merged.resize(kSnapshotLimit);
        }
        out = std::move(merged);
    };
    fill(bids_, bid_overlay, true, s.bids);
    fill(asks_, ask_overlay, false, s.asks);
    return s;
}

bool ExchangeSim::coverage_sufficient(const DepthSnapshot& served) const {
    const auto side_ok = [](const auto& truth, const std::vector<Level>& served_side, bool bids) {
        if (served_side.size() < kSnapshotLimit) {
            return true;  // complete side
        }
        const std::int64_t boundary = served_side.back().price;
        int count = 0;
        for (const auto& [price, qty] : truth) {
            (void)qty;
            const bool inside = bids ? price >= boundary : price <= boundary;
            if (!inside) {
                break;
            }
            if (++count >= 5) {
                return true;
            }
        }
        return false;
    };
    return side_ok(bids_, served.bids, true) && side_ok(asks_, served.asks, false);
}

}  // namespace blc::test
